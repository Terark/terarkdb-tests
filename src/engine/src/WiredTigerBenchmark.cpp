//
// Created by terark on 8/24/16.
//

#include "WiredTigerBenchmark.h"
#include "../port/port_posix.h"
#include <terark/util/autoclose.hpp>
#include <terark/util/linebuf.hpp>
#include <fstream>

struct WT_ThreadState : public ThreadState {
  WT_SESSION* session = nullptr;
  WT_CURSOR*  cursor  = nullptr;

  WT_ThreadState(int index, WT_CONNECTION *conn, const char* uri,
                 const std::atomic<std::vector<bool>*>* wsp)
    : ThreadState(index, wsp)
  {
    conn->open_session(conn, NULL, NULL, &session);
    STOP.store(false);
    assert(session != NULL);
    int ret = session->open_cursor(session, uri, NULL, NULL, &cursor);
    if (ret != 0) {
        fprintf(stderr, "open_cursor error: %s\n", wiredtiger_strerror(ret));
        exit(1);
    }
  }
  ~WT_ThreadState() {
    if (cursor)
      cursor->close(cursor);
    if (session)
      session->close(session, NULL);
  }
};

WiredTigerBenchmark::WiredTigerBenchmark(Setting& setting1)
  : Benchmark(setting1)
{
  char uri[100];
  snprintf(uri, sizeof(uri), "%s:dbbench_wt-%d",
           setting.FLAGS_use_lsm ? "lsm" : "table", db_num_);
  uri_ = uri;
}

WiredTigerBenchmark::~WiredTigerBenchmark() {
}

ThreadState*
WiredTigerBenchmark::newThreadState(const std::atomic<std::vector<bool>*>* whichSPlan) {
    return new WT_ThreadState(threads.size(), conn_, uri_.c_str(), whichSPlan);
}

void WiredTigerBenchmark::Load(void) {
    DoWrite(true);
}

void WiredTigerBenchmark::Close(void) {
    clearThreads();
    conn_->close(conn_, NULL);
    conn_ = NULL;
}

bool WiredTigerBenchmark::ReadOneKey(ThreadState *thread){
    if (!getRandomKey(thread->key, thread->randGenerator)){
        return false;
    }
    auto t = static_cast<WT_ThreadState*>(thread);
    WT_CURSOR *cursor = t->cursor;
    cursor->set_key(cursor, thread->key.c_str());
    if (cursor->search(cursor) == 0) {
        const char* val;
        int ret = cursor->get_value(cursor, &val);
        cursor->reset(cursor);
        return 0 == ret;
    }
    cursor->reset(cursor);
    return false;
}

bool WiredTigerBenchmark::UpdateOneKey(ThreadState *thread){
    auto t = static_cast<WT_ThreadState*>(thread);
    std::string &rkey = thread->key;
    if (!getRandomKey(rkey,thread->randGenerator)){
        return false;
    }
    WT_CURSOR *cursor = t->cursor;
    cursor->set_key(cursor, rkey.c_str());
    if (cursor->search(cursor) != 0){
        fprintf(stderr, "ERROR: cursor search: %s\n", rkey.c_str());
        return false;
    }
    const char *val;
    int ret = cursor->get_value(cursor,&val);
    assert(ret == 0);
    cursor->set_value(cursor,val);
    ret = cursor->insert(cursor);
    if (ret != 0){
        fprintf(stderr, "ERROR: cursor insert: %s\n", rkey.c_str());
        return false;
    }
    return true;
}

bool WiredTigerBenchmark::VerifyOneKey(ThreadState *thread) {
    auto t = static_cast<WT_ThreadState *>(thread);
    std::string &rkey = thread->key;
    std::string &rval = thread->value;
    std::string &rstr = thread->str;
    WT_CURSOR *cursor = t->cursor;
    if (!verifyDataCq.try_pop(rstr)) {
        return false;
    }
    int ret = setting.splitKeyValue(rstr, &rkey, &rval);
    if (ret == 0) {
        return false;
    }
    cursor->set_key(cursor, thread->str.c_str());
    if (cursor->search(cursor) == 0) {
        std::string val;
        int ret = cursor->get_value(cursor, &val);
        if (ret == 0) {
            if (rval == val) {
                thread->verifyResult = VERIFY_TYPE::MATCH;
            } else {
                thread->verifyResult = VERIFY_TYPE::MISMATCH;
                thread->storeValue = val;
            }
        } else {
            thread->verifyResult = VERIFY_TYPE::FAIL;
        }
        return true;
    } else {
        return false;
    }
}

bool WiredTigerBenchmark::InsertOneKey(ThreadState *thread){
    auto t = static_cast<WT_ThreadState*>(thread);
    std::string &rkey = thread->key;
    std::string &rval = thread->value;
    std::string &rstr = thread->str;
    WT_CURSOR *cursor = t->cursor;
    if (!updateDataCq.try_pop(rstr)) {
        return false;
    }
    int ret = setting.splitKeyValue(rstr, &rkey, &rval);
    if (ret == 0) {
        return false;
    }
    cursor->set_key(cursor, rkey.c_str());
    cursor->set_value(cursor, rval.c_str());
    ret = cursor->insert(cursor);
    if (ret != 0) {
        fprintf(stderr, "insert error: %s\n", wiredtiger_strerror(ret));
        return false;
    }
    return true;
}
void WiredTigerBenchmark::Open(){
    PrintHeader();
    PrintEnvironment();
    PrintWarnings();
#define SMALL_CACHE 10*1024*1024
    std::stringstream config;
    config.str("");
    if (!setting.FLAGS_use_existing_db) {
        config << "create";
    }
    if (setting.FLAGS_cache_size > 0)
        config << ",cache_size=" << setting.FLAGS_cache_size;
    config << ",log=(enabled,recover=on)";
    config << ",checkpoint=(log_size=64MB,wait=60)";
    config << ",extensions=[libwiredtiger_snappy.so]";
    //config << ",verbose=[lsm]";
    auto strConf = config.str();
    system(("mkdir -p " + setting.FLAGS_db).c_str());
    fprintf(stderr, "INFO: wiredtiger_open(db=%s, conf=%s)\n"
                  , setting.FLAGS_db.c_str(), strConf.c_str());
    int err = wiredtiger_open(setting.FLAGS_db.c_str(), NULL, strConf.c_str(), &conn_);
    if (err) {
      fprintf(stderr, "ERROR: wiredtiger_open(, %s) = %s\n"
                    , setting.FLAGS_db.c_str(), wiredtiger_strerror(err));
      exit(1);
    }
    assert(conn_ != NULL);

    WT_SESSION *session;
    err = conn_->open_session(conn_, NULL, NULL, &session);
    if (err) {
      fprintf(stderr, "ERROR: conn_->open_session(%s) = %s\n"
                    , setting.FLAGS_db.c_str(), wiredtiger_strerror(err));
      exit(1);
    }
    assert(session != NULL);

    if (!setting.FLAGS_use_existing_db) {
        config.str("");
        config << "key_format=S,value_format=S";
        config << ",columns=[p,val]";
        config << ",prefix_compression=true";
        config << ",checksum=off";
        config << ",internal_page_max=16kb";
        config << ",leaf_page_max=" << setting.FLAGS_block_size;
    //  config << ",memory_page_max=" << setting.FLAGS_cache_size;
        if (setting.FLAGS_use_lsm) {
            config << ",lsm=(";
            if (setting.FLAGS_cache_size > SMALL_CACHE)
                config << "chunk_size=" << setting.FLAGS_write_buffer_size;
            if (setting.FLAGS_bloom_bits > 0)
                config << ",bloom_bit_count=" << setting.FLAGS_bloom_bits;
            else if (setting.FLAGS_bloom_bits == 0)
                config << ",bloom=false";
            config << ")";
        }
        config << ",block_compressor=snappy";
        strConf = config.str();
        fprintf(stderr, "session->create(uri=%s, conf=%s)\n", uri_.c_str(), strConf.c_str());
        int ret = session->create(session, uri_.c_str(), strConf.c_str());
        if (ret != 0) {
            fprintf(stderr, "create error: %s\n", wiredtiger_strerror(ret));
            exit(1);
        }
        session->close(session, NULL);
    }
}

bool WiredTigerBenchmark::Compact() {
  WT_SESSION *session;
  int ret = conn_->open_session(conn_, NULL, NULL, &session);
  if (0 == ret) {
    fprintf(stderr, "INFO: WiredTigerBenchmark::Compact()...\n");
    session->compact(session, uri_.c_str(), NULL);
    session->close(session, NULL);
    fprintf(stderr, "INFO: WiredTigerBenchmark::Compact()...done!\n");
    return true;
  }
  fprintf(stderr, "ERROR: WiredTigerBenchmark::Compact() failed\n");
  return false;
}

void WiredTigerBenchmark::DoWrite(bool seq) {
    fprintf(stderr, "WiredTigerBenchmark::DoWrite(%d) start\n", seq);
    std::stringstream txn_config;
    txn_config.str("");
    txn_config << "isolation=snapshot";
    if (sync_)
        txn_config << ",sync=full";
    else
        txn_config << ",sync=none";
    WT_CURSOR *cursor;
    std::stringstream cur_config;
    cur_config.str("");
    cur_config << "overwrite";
    WT_SESSION *session;
    conn_->open_session(conn_, NULL, NULL, &session);
    int ret = session->open_cursor(session, uri_.c_str(), NULL, cur_config.str().c_str(), &cursor);
    if (ret != 0) {
        fprintf(stderr, "ERROR: open_cursor: %s\n", wiredtiger_strerror(ret));
        exit(1);
    }
    long long recordnumber = 0;
    terark::Auto_close_fp file(fopen(setting.getLoadDataPath().c_str(), "r"));
    if (!file) {
      fprintf(stderr, "ERROR: fopen(%s, r) = %s\n"
          , setting.getLoadDataPath().c_str(), strerror(errno));
      exit(1);
    }
    terark::LineBuf line;
    std::string key;
    std::string val;
    std::mt19937_64 random;
    auto randomUpper = uint64_t(0.01 * random.max() * setting.getSamplingRate());
    while (line.getline(file) > 0) {
        ret = setting.splitKeyValue(line, &key, &val);
        if (ret == 0)
            continue;
        cursor->set_key(cursor, key.c_str());
        cursor->set_value(cursor,val.c_str());
        int ret;
        if (random() < randomUpper) {
            struct timespec beg, end;
            clock_gettime(CLOCK_REALTIME, &beg);
            ret = cursor->insert(cursor);
            clock_gettime(CLOCK_REALTIME, &end);
            Stats::FinishedSingleOp(OP_TYPE::INSERT, beg, end);
        }
        else {
            ret = cursor->insert(cursor);
        }
        if (ret != 0) {
            fprintf(stderr, "set error: %s\n", wiredtiger_strerror(ret));
            exit(1);
        }
        recordnumber++;
        if (recordnumber % 100000 == 0)
            fprintf(stderr, "Record number: %lld\n", recordnumber);
    }
    cursor->close(cursor);
    session->close(session, NULL);
    time_t now;
    struct tm *timenow;
    time(&now);
    timenow = localtime(&now);
    fprintf(stderr, "recordnumber %lld,  time %s\n",recordnumber, asctime(timenow));
}

void WiredTigerBenchmark::PrintHeader() {
    const int kKeySize = 16;
    PrintEnvironment();
    fprintf(stdout, "Keys:       %d bytes each\n", kKeySize);
    fprintf(stdout, "Values:     %d bytes each (%d bytes after compression)\n",
            setting.FLAGS_value_size,
            static_cast<int>(setting.FLAGS_value_size * setting.FLAGS_compression_ratio + 0.5));
    fprintf(stdout, "Entries:    %d\n", num_);
    fprintf(stdout, "RawSize:    %.1f MB (estimated)\n",
            ((static_cast<int64_t>(kKeySize + setting.FLAGS_value_size) * num_)
             / 1048576.0));
    fprintf(stdout, "FileSize:   %.1f MB (estimated)\n",
            (((kKeySize + setting.FLAGS_value_size * setting.FLAGS_compression_ratio) * num_)
             / 1048576.0));
    PrintWarnings();
    fprintf(stdout, "------------------------------------------------\n");
}

void WiredTigerBenchmark::PrintWarnings() {
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
    fprintf(stdout,
            "WARNING: Optimization is disabled: benchmarks unnecessarily slow\n"
    );
#endif
#ifndef NDEBUG
    fprintf(stdout,
            "WARNING: Assertions are enabled; benchmarks unnecessarily slow\n");
#endif

    // See if snappy is working by attempting to compress a compressible string
    const char text[] = "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy";
    std::string compressed;
    if (!leveldb::port::Snappy_Compress(text, sizeof(text), &compressed)) {
        fprintf(stdout, "WARNING: Snappy compression is not enabled\n");
    } else if (compressed.size() >= sizeof(text)) {
        fprintf(stdout, "WARNING: Snappy compression is not effective\n");
    }
}

void WiredTigerBenchmark::PrintEnvironment() {
    int wtmaj, wtmin, wtpatch;
    const char *wtver = wiredtiger_version(&wtmaj, &wtmin, &wtpatch);
    fprintf(stdout, "WiredTiger:    version %s, lib ver %d, lib rev %d patch %d\n",
            wtver, wtmaj, wtmin, wtpatch);
    fprintf(stderr, "WiredTiger:    version %s, lib ver %d, lib rev %d patch %d\n",
            wtver, wtmaj, wtmin, wtpatch);

#if defined(__linux)
    time_t now = time(NULL);
    fprintf(stderr, "Date:       %s", ctime(&now));  // ctime() adds newline
    FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo != NULL) {
        char line[1000];
        int num_cpus = 0;
        std::string cpu_type;
        std::string cache_size;
        while (fgets(line, sizeof(line), cpuinfo) != NULL) {
            const char* sep = strchr(line, ':');
            if (sep == NULL) {
                continue;
            }
            fstring key(line, sep - 1 - line);
            fstring val(sep + 1);
            key.trim();
            val.trim();
            if (key == "model name") {
                ++num_cpus;
                cpu_type = val.str();
            } else if (key == "cache size") {
                cache_size = val.str();
            }
        }
        fclose(cpuinfo);
        fprintf(stderr, "CPU:        %d * %s\n", num_cpus, cpu_type.c_str());
        fprintf(stderr, "CPUCache:   %s\n", cache_size.c_str());
    }
#endif
}
