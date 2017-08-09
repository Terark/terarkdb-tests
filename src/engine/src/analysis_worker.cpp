//
// Created by terark on 16-8-11.
//
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h> // for extern "C" char **environ;
#include "analysis_worker.h"
#include "util/system_resource.h"
#include <terark/io/MemStream.hpp>
#include <terark/lcast.hpp>
//#include <terark/util/autoclose.hpp>
#include <mysql.h>
#include <errmsg.h>
#include <fstream>

using terark::lcast;
using terark::fstring;

class TimeBucket {
private:
    const char* engine_name;
    const std::vector<std::string>& dbdirs;

    int current_bucket = 0;  // seconds
    int operation_count = 0;

public:
    TimeBucket(const char* engineName,
               const std::vector<std::string>& dbdirs)
            : engine_name(engineName),
              dbdirs(dbdirs)
    {}

    void update_ops(terark::AutoGrownMemIO& buf, int sampleRate, OP_TYPE opType);
};

static int findTimeBucket(uint64_t time) {
    static const uint64_t step_in_seconds = 10; // in seconds
    uint64_t t = time / (1000 * 1000 * 1000 * step_in_seconds);
    // fprintf(stderr, "find time bucket : %" PRIu64 ", result = %" PRIu64 "\n", time, t*10);
    return t*10;
}

char* g_passwd = getenv("MYSQL_PASSWD");
static MYSQL g_conn;
static bool  g_hasConn = false;
static const int g_latencyTimePeriodsUS[] = {
        10,
        20,
        30,
        50,
       100,
       200,
       300,
       500,
      1000, //    1ms
      2000,
      3000,
      5000,
     10000, //   10ms
     20000,
     30000,
     50000,
    100000, //  100ms
    200000,
    300000,
    500000,
   1000000, // 1000ms, 1s
   2000000,
   3000000,
   5000000,
   INT_MAX,
};
#define dimof(array) (sizeof(array)/sizeof(array[0]))

struct LatencyStat {
    int cnts[dimof(g_latencyTimePeriodsUS)];
    LatencyStat() : cnts{0} {}
    void reset() { memset(cnts, 0, sizeof(cnts)); }
    void update(std::pair<uint64_t, uint64_t> tt);
};
void LatencyStat::update(std::pair<uint64_t, uint64_t> tt) {
  int latencyUS = int((tt.second - tt.first) * 0.001);
  size_t idx = terark::lower_bound_0(g_latencyTimePeriodsUS, dimof(g_latencyTimePeriodsUS), latencyUS);
  cnts[idx]++;
}
LatencyStat g_latencyStat;

static bool Mysql_connect(MYSQL* conn) {
    if(g_passwd == NULL || strlen(g_passwd) == 0) {
        fprintf(stderr, "no MYSQL_PASSWD set, analysis thread will not upload data!\n");
        return false;
    }
    fprintf(stderr, "Mysql_connect, passwd = %s\n", g_passwd);
    const char* host = "rds432w5u5d17qd62iq3o.mysql.rds.aliyuncs.com";
    const char* user = "terark_benchmark";
    const char* db = "benchmark";
    int port = 3306;
    if (const char* env = getenv("MYSQL_SERVER")) {
        host = env;
    }
    if (const char* env = getenv("MYSQL_USER")) {
        user = env;
    }
    if (const char* env = getenv("MYSQL_PORT")) {
        port = atoi(env);
    }
    my_bool myTrue = true;
    mysql_init(conn);
    mysql_options(conn, MYSQL_OPT_RECONNECT, &myTrue); // brain dead reconnect has a fucking bug
    conn->reconnect = true;
    unsigned long clientflag = CLIENT_REMEMBER_OPTIONS;
    if (!mysql_real_connect(conn, host, user, g_passwd, db, port, NULL, clientflag)) {
	//fprintf(stderr, "ERROR: mysql_real_connect failed\n");
	//return false;
        fprintf(stderr
                , "ERROR: mysql_real_connect(host=%s, user=%s, passwd=%s, db=%s, port=%d, NULL, CLIENT_REMEMBER_OPTIONS) = %s\n"
                  "       database connection fault, monitor data will not be uploaded\n"
                , host, user, g_passwd, db, port, mysql_error(conn)
        );
        return false;
    }
    fprintf(stderr, "database connected!\n");
    return true;
}

static MYSQL_STMT* prepare(MYSQL* conn, fstring sql) {
    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    int err = mysql_stmt_prepare(stmt, sql.data(), sql.size());
    if (err) {
        fprintf(stderr, "ERROR: mysql_stmt_prepare(%s) = %s\n", sql.c_str(), mysql_error(conn));
        return NULL;
    }
    return stmt;
}

// also write monitor stat to file
static std::ofstream ofs_ops, ofs_memory, ofs_cpu, ofs_dbsize, ofs_diskinfo, ofs_latency;
static bool open_stat_files(std::string fprefix) {
  auto open1 = [&](std::ofstream& ofs, const char* suffix) -> bool {
    std::string fpath = fprefix + "-" + suffix + ".txt";
    ofs.open(fpath);
    if (!ofs.is_open()) {
      fprintf(stderr, "ERROR: ofstream(%s) = %s\n", fpath.c_str(), strerror(errno));
      return false;
    }
    return true;
  };
  return 1
  && open1(ofs_ops     , "ops"     )
  && open1(ofs_memory  , "memory"  )
  && open1(ofs_cpu     , "cpu"     )
  && open1(ofs_dbsize  , "dbsize"  )
  && open1(ofs_diskinfo, "diskinfo")
  && open1(ofs_latency , "latency" )
  ;
}

static std::ofstream ofs_verify;
static bool open_verify_file(std::string fprefix) {
  std::string fpath = fprefix + "-verify-fail.txt";
  ofs_verify.open(fpath);
  if(!ofs_verify.is_open()) {
    fprintf(stderr, "ERROR: ofstream(%s) = %s\n", fpath.c_str(), strerror(errno));
    return false;
  }
  return true;
}

static MYSQL_STMT *ps_ops, *ps_memory, *ps_cpu, *ps_dbsize, *ps_diskinfo, *ps_latency;
static void prepair_all_stmt() {
    ps_ops = prepare(&g_conn, "INSERT INTO engine_test_ops_10s(time_bucket, ops, ops_type, engine_name) VALUES(?, ?, ?, ?)");
    ps_memory = prepare(&g_conn, "INSERT INTO engine_test_memory_10s(time_bucket, total_memory, free_memory, cached_memory, used_memory, engine_name) VALUES(?, ?, ?, ?, ?, ?)");
    ps_cpu = prepare(&g_conn, "INSERT INTO engine_test_cpu_10s(time_bucket, `usage`, `iowait`, engine_name) VALUES(?, ?, ?, ?)");
    ps_dbsize = prepare(&g_conn, "INSERT INTO engine_test_dbsize_10s(time_bucket, `dbsize`, `engine_name`) VALUES(?, ?, ?)");
    ps_diskinfo = prepare(&g_conn, "INSERT INTO engine_test_diskinfo_10s(time_bucket, `diskinfo`, `engine_name`) VALUES(?, ?, ?)");
    ps_latency = prepare(&g_conn, "INSERT INTO engine_test_latency_10s(time_bucket, op_type, latency_us, cnt, `engine_name`) VALUES(?, ?, ?, ?, ?)");
}

void Bind_arg(MYSQL_BIND &b, const int &val) {
    memset(&b, 0, sizeof(b));
    b.buffer_length = 4;
    b.buffer_type = MYSQL_TYPE_LONG;
    b.buffer = (void*)&val;
}
void Bind_arg(MYSQL_BIND& b, const double& val) {
    memset(&b, 0, sizeof(b));
    b.buffer_length = 4;
    b.buffer_type = MYSQL_TYPE_DOUBLE;
    b.buffer = (void*)&val;
}
void Bind_arg(MYSQL_BIND& b, fstring val) {
    memset(&b, 0, sizeof(b));
    b.buffer_length = val.size();
    b.buffer_type = MYSQL_TYPE_VAR_STRING;
    b.buffer = (void*)val.data();
}

int Escape_arg(const int &val) {
  return val;
}
double Escape_arg(const double& val) {
  return val;
}
std::string Escape_arg(fstring val) {
  std::string esc(val.size()*3, '\0');
  size_t len = mysql_escape_string(&esc[0], val.data(), val.size());
  esc.resize(len);
  return esc;
}

template<class... Args>
bool Exec_stmt(st_mysql_stmt* stmt, const Args&... args) {
    MYSQL_BIND  b[sizeof...(Args)];
    memset(&b, 0, sizeof(b));
    int i = 0;
    std::initializer_list<int>{(Bind_arg(b[i], args), i++)...};
    mysql_stmt_bind_param(stmt, b);
    int err = mysql_stmt_execute(stmt);
    if (err) {
        fprintf(stderr, "WARN: %s = %s\n", BOOST_CURRENT_FUNCTION, stmt->last_error);
        if (CR_SERVER_LOST == err) {
        //  mysql_ping(stmt->mysql); // brain dead mysql
            Mysql_connect(&g_conn);
            prepair_all_stmt();
        }
        return false;
    }
    return true;
}

template<class... Args>
bool Exec_stmt(std::ofstream& ofs, st_mysql_stmt* stmt, const Args&... args) {
    if (ofs.is_open()) {
        size_t  i = 0;
        const size_t a[]{(
            i + 1 < sizeof...(Args) ? ofs << Escape_arg(args) : ofs,
            i + 2 < sizeof...(Args) ? ofs << "," : ofs,
            i++
        )...};
        ofs << "\n";
        (void)(a);
        return !g_hasConn;
    }
    else if (!g_hasConn) {
        return false;
    }
    return Exec_stmt(stmt, args...);
}

static int g_prev_sys_stat_bucket = 0;
static void upload_sys_stat(terark::AutoGrownMemIO& buf,
                            const std::vector<std::string>& dbdirs,
                            int bucket, const char* engine_name) {
// 顺便把CPU等数据也上传, 相同时间片只需要上传一次即可
    if (bucket == g_prev_sys_stat_bucket) {
        return;
    }
    g_prev_sys_stat_bucket = bucket;
    int arr[4];
    benchmark::getPhysicalMemoryUsage(arr);
    Exec_stmt(ofs_memory, ps_memory, bucket, arr[0], arr[1], arr[2], arr[3], engine_name);
    buf.printf("    total memory = %5.2f GiB", arr[0]/1024.0);

    double cpu[2];
    benchmark::getCPUPercentage(cpu);
    if(cpu > 0){
        Exec_stmt(ofs_cpu, ps_cpu, bucket, cpu[0], cpu[1], engine_name);
    }
    buf.printf("    cpu usage = %5.2f iowait = %5.2f", cpu[0], cpu[1]);

    int dbsizeKB = benchmark::getDiskUsageByKB(dbdirs);
    if(dbsizeKB > 0) {
        Exec_stmt(ofs_dbsize, ps_dbsize, bucket, dbsizeKB, engine_name);
    }
    buf.printf("    dbsize = %5.2f GiB", dbsizeKB/1024.0/1024);

    std::string diskinfo;
    benchmark::getDiskFileInfo(dbdirs, diskinfo);
    if(diskinfo.length() > 0) {
        Exec_stmt(ofs_diskinfo, ps_diskinfo, bucket, fstring(diskinfo), engine_name);
    }
}

void TimeBucket::update_ops(terark::AutoGrownMemIO& buf, int sampleRate, OP_TYPE opType) {
    std::pair<uint64_t, uint64_t> tt;
    const int type = int(opType) + 1;
	  int loop_cnt = 0;
    while (Stats::opsDataCq[int(opType)].try_pop(tt)) {
      loop_cnt++;
      int next_bucket = findTimeBucket(tt.first);
      if (next_bucket > current_bucket) {
          // when meet the next bucket, upload previous one first, default step is 10 seconds
          // * 100 / (10 * sampleRate) // sampleRate : (0, 100]
          int ops = int(operation_count * 10.0 / sampleRate);
          buf.rewind();
          Exec_stmt(ofs_ops, ps_ops, current_bucket, ops, type, engine_name);
          for (size_t i = 0; i < dimof(g_latencyStat.cnts); ++i) {
              auto latency = g_latencyTimePeriodsUS[i];
              auto cnt = int(g_latencyStat.cnts[i] * 100.0 / sampleRate);
              if (cnt) {
                  Exec_stmt(ofs_latency, ps_latency,
                      current_bucket, type, latency, cnt, engine_name);
              }
          }
          buf.printf("upload statistic time bucket[%d], ops = %7d, type = %d, loop = %7d"
        , current_bucket, ops, type, loop_cnt);
          upload_sys_stat(buf, dbdirs, current_bucket, engine_name);
          fprintf(stderr, "%s\n", buf.begin());
          g_latencyStat.reset();
          g_latencyStat.update(tt);
          operation_count = 1;
          current_bucket = next_bucket;
          break;
      } else {
          operation_count++;
          g_latencyStat.update(tt);
      }
    }
}

static void write_verify_fail_data() {
    std::string str;
    while (Stats::verifyFailDataCq.try_pop(str)) {
      if (ofs_verify.is_open()) {
        ofs_verify << str;
      }
    }
}

AnalysisWorker::AnalysisWorker(Setting* setting) {
    this->setting = setting;
}

AnalysisWorker::~AnalysisWorker() {
    fprintf(stderr, "analysis worker is stopped!\n");
}

void AnalysisWorker::stop() {
    shoud_stop = true;
}

char** g_argv;
int g_argc;

void upload_command_and_env(fstring engine_name) {
  MYSQL_STMT* stmt = prepare(&g_conn, "insert into engine_test_command(engine_name, time, command_line, env) values (?,?,?,?)");
  std::string cmd, env;
  for (char** ppEnv = environ; *ppEnv; ++ppEnv) {
    fstring arg = *ppEnv;
    if (!arg.startsWith("LS_COLORS=")) {
      env.append(arg.data(), arg.size());
      env += "\n";
    }
  }
  for (int i = 0; i < g_argc; ++i) {
    fstring arg = g_argv[i];
    if (arg.startsWith("--mysql_passwd")) {
      cmd.append("--mysql_passwd=******\n");
    }
    else {
      cmd.append(arg.data(), arg.size());
      cmd += "\n";
    }
  }
  time_t now = time(NULL);
  Exec_stmt(stmt, engine_name, int(now), cmd, env);
  mysql_stmt_close(stmt);
}

void AnalysisWorker::run() {
    if (const char* fprefix = getenv("MONITOR_STAT_FILE_PREFIX")) {
        if (!open_stat_files(fprefix)) {
          return;
        }
    }
    if (setting->getAction() == "verify") {
        std::string fprefix;
        if (const char* env_fprefix = getenv("MONITOR_STAT_FILE_PREFIX")) {
          fprefix = env_fprefix;
        }
        else {
          fprefix = "benchmark";
        }
        if (!open_verify_file(fprefix)) {
          return ;
        }
    }
    g_hasConn = Mysql_connect(&g_conn);
    if (!g_hasConn) {
        if (!ofs_ops.is_open()) {
            return;
        }
    }
    else {
  #if 0
      struct timespec t;
      clock_gettime(CLOCK_REALTIME, &t);
      int filter_time = t.tv_sec - 60*60*24*60;
      std::string tables[] = {"engine_test_ops_10s",
                              "engine_test_memory_10s",
                              "engine_test_cpu_10s",
                              "engine_test_dbsize_10s",
                              "engine_test_diskinfo_10s",
                              "engine_test_latency_10s",
      };
      for(std::string& table: tables) {
          std::string sql = "DELETE FROM " + table + " WHERE time_bucket < " + lcast(filter_time);
          mysql_real_query(&g_conn, sql.c_str(), sql.size());
      }
  #endif
      prepair_all_stmt();
      upload_command_and_env(engine_name);
    }

    std::pair<uint64_t, uint64_t> search_result, insert_result, update_result;
    TimeBucket search_bucket(engine_name.c_str(), setting->dbdirs);
    TimeBucket insert_bucket(engine_name.c_str(), setting->dbdirs);
    TimeBucket update_bucket(engine_name.c_str(), setting->dbdirs);
    terark::AutoGrownMemIO buf;
    shoud_stop = false;
    while(!shoud_stop) {
        auto samplingRate = setting->getSamplingRate();
        search_bucket.update_ops(buf, samplingRate, OP_TYPE::SEARCH);
        insert_bucket.update_ops(buf, samplingRate, OP_TYPE::INSERT);
        update_bucket.update_ops(buf, samplingRate, OP_TYPE::UPDATE);
        if (setting->getAction() == "verify") {
          write_verify_fail_data();
        }
        timespec ts1;
        clock_gettime(CLOCK_REALTIME, &ts1);
        unsigned long long tt = 1000000000ull * ts1.tv_sec + ts1.tv_nsec;
        int curr_bucket = findTimeBucket(tt);
        if (curr_bucket > g_prev_sys_stat_bucket) {
            buf.rewind();
            buf.printf("upload statistic time bucket[%d], nop", curr_bucket);
            upload_sys_stat(buf, setting->dbdirs, curr_bucket, engine_name.c_str());
            fprintf(stderr, "%s\n", buf.begin());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (g_hasConn) {
        mysql_stmt_close(ps_ops);
        mysql_stmt_close(ps_memory);
        mysql_stmt_close(ps_cpu);
        mysql_stmt_close(ps_dbsize);
        mysql_stmt_close(ps_diskinfo);
        mysql_stmt_close(ps_latency);
        mysql_close(&g_conn);
    }
}
