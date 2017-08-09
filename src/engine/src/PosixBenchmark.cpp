//
// Created by terark on 9/10/16.
//

#include <dirent.h>
#include "PosixBenchmark.h"
#include <terark/util/autoclose.hpp>
#include <terark/io/FileStream.hpp>

using terark::FileStream;

PosixBenchmark::PosixBenchmark(Setting& setting):Benchmark(setting){
}

void PosixBenchmark::Open(void) {
    DIR *dir;
    dir = opendir(setting.FLAGS_db.c_str());
    if (dir == NULL){
        throw std::invalid_argument(strerror(errno));
    }
    auto ret = closedir(dir);
    if ( ret != 0){
        throw std::invalid_argument(strerror(errno));
    }
}

void PosixBenchmark::Close(void) {
}

void PosixBenchmark::Load(void) {
    DIR *dir;
    fprintf(stderr, "PosixBenchmakr::Load to : %s\n", setting.getLoadDataPath().c_str());
    dir = opendir(setting.getLoadDataPath().c_str());
    if (dir == NULL){
        throw std::invalid_argument(strerror(errno) + setting.getLoadDataPath());
    }
    int ret = closedir(dir);
    if (ret != 0){
        throw std::invalid_argument(strerror(errno) + setting.getLoadDataPath());
    }
    if (system(NULL) == 0)
        throw std::logic_error("system error:no bash to use!\n");

    std::string cp_cmd = "cp " + setting.getLoadDataPath() + " " + setting.FLAGS_db + "/" + " -r";
    fprintf(stderr, "%s\n", cp_cmd.c_str());
    ret = system(cp_cmd.c_str());
    if (ret == -1)
        throw std::runtime_error("cp_cmd execute failed,child ,can't create child process.\n");
    if (ret == 127)
        throw std::runtime_error("cp_cmd execute failed!\n");
    return ;
}

bool PosixBenchmark::ReadOneKey(ThreadState *ts) {
    if (!getRandomKey(ts->key, ts->randGenerator))
        return false;
    std::string read_path = setting.FLAGS_db;
    read_path = read_path + "/" + ts->key;
    FileStream read_file(read_path, "r");
    size_t size = read_file.fsize();
    std::unique_ptr<char> buf(new char [size]);
    if (size != read_file.read(buf.get(), size)){
        fprintf(stderr,"posix read error:%s\n",strerror(errno));
        return false;
    }
    return true;
}

bool PosixBenchmark::UpdateOneKey(ThreadState *ts) {
    if (!getRandomKey(ts->key, ts->randGenerator)){
        fprintf(stderr,"RocksDbBenchmark::UpdateOneKey:getRandomKey false\n");
        return false;
    }
    ts->key = setting.FLAGS_db + ts->key;
    std::unique_ptr<FILE, decltype(&fclose)> update_file(fopen(ts->key.c_str(),"r"),fclose);
    size_t size = FileStream::fpsize(update_file.get());
    std::unique_ptr<char> buf(new char[size]);
    if (size != fread(buf.get(),1,size,update_file.get())){
        fprintf(stderr,"posix update read error:%s\n",strerror(errno));
        return false;
    }
    if (size != fwrite(buf.get(),1,size,update_file.get())){
        fprintf(stderr,"posix update write error:%s\n",strerror(errno));
        return false;
    }
    return true;
}

bool PosixBenchmark::InsertOneKey(ThreadState *ts) {
    if (!updateDataCq.try_pop(ts->key)) {
        return false;
    }
    ts->str = setting.getInsertDataPath() + "/" + ts->key;
    std::unique_ptr<FILE, decltype(&fclose)> insert_source_file(fopen(ts->str.c_str(),"r"),fclose);
    ts->str = setting.FLAGS_db;
    ts->str = ts->str + "/" + ts->key;
    std::unique_ptr<FILE, decltype(&fclose)> insert_target_file(fopen(ts->str.c_str(),"w+"),fclose);
    size_t size = FileStream::fpsize(insert_source_file.get());
    std::unique_ptr<char> buf(new char[size]);
    if (size != fread(buf.get(),1,size,insert_source_file.get())){
        fprintf(stderr,"posix insert read error:%s\n",strerror(errno));
        return false;
    }
    if (size != fwrite(buf.get(),1,size,insert_target_file.get())){
        fprintf(stderr,"posix insert write error:%s\n",strerror(errno));
        return false;
    }
    return true;
}

bool PosixBenchmark::VerifyOneKey(ThreadState *ts) {
    if (!verifyDataCq.try_pop(ts->key)) {
        return false;
    }

    ts->str = setting.getVerifyKvFile() + "/" + ts->key;
    std::unique_ptr<FILE, decltype(&fclose)> verify_source_file(fopen(ts->str.c_str(), "r"), fclose);

    ts->str = setting.FLAGS_db + "/" + ts->key;
    std::unique_ptr<FILE, decltype(&fclose)> verify_target_file(fopen(ts->str.c_str(), "r"), fclose);

    if (verify_target_file.get() == nullptr) {
        ts->verifyResult = VERIFY_TYPE::FAIL;
        return true;
    }

    size_t source_size = FileStream::fpsize(verify_source_file.get());
    size_t target_size = FileStream::fpsize(verify_target_file.get());

    std::unique_ptr<char> source_buf(new char[source_size]);
    std::unique_ptr<char> target_buf(new char[target_size]);

    if (source_size != fread(source_buf.get(), 1, source_size, verify_source_file.get())) {
        fprintf(stderr, "posix verify read source error:%s\n", strerror(errno));
        return false;
    }

    if (source_size == target_size) {
        if (target_size != fread(target_buf.get(), 1, target_size, verify_target_file.get())) {
            fprintf(stderr, "posix verify read target error:%s\n", strerror(errno));
            return false;
        }

        if (strcmp(source_buf.get(), target_buf.get()) == 0) {
            ts->verifyResult = VERIFY_TYPE::MATCH;
        } else {
            ts->storeValue = std::string(source_buf.get());
            ts->verifyResult = VERIFY_TYPE::MISMATCH;
        }
        return true;
    } else {
        ts->storeValue = std::string(source_buf.get());
        ts->verifyResult = VERIFY_TYPE::MISMATCH;
        return true;
    }
}

bool PosixBenchmark::Compact(void) {
    return false;
}

ThreadState *PosixBenchmark::newThreadState(const std::atomic<std::vector<bool>*>* whichSamplingPlan) {
    return new ThreadState(threads.size(), whichSamplingPlan);
}



