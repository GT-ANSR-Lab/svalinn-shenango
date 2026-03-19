#include <rocksdb/db.h>

#include <getopt.h>
#include <cstdio>
#include <string>
#include <iostream>
#include <vector>
#include <unistd.h>
#include <thread>
#include <random>
#include <cmath>

// Program settings
struct {
    std::string db_path = "";
    int lo_skey_size = 0;
    int hi_skey_size = 0;
    int skey_count = 0;
    int lo_lkey_size = 0;
    int hi_lkey_size = 0;
    int lkey_count = 0;
    int nb_threads = 16;
} g_settings;


// Help message
void Help() {
    std::cerr << "Usage: rocksdb_createdb --db_path <Path to tmpfs rocksdb instance> " << std::endl
              << "       --lo_skey_size <Minimum size of short keys> " << std::endl
              << "       --hi_skey_size <Maximum size of short keys> " << std::endl
              << "       --skey_count <Number of short keys> " << std::endl
              << "       --lo_lkey_size <Minimum size of long keys> " << std::endl
              << "       --hi_lkey_size <Maximum size of long keys> " << std::endl
              << "       --lkey_count <Number of large keys> " << std::endl
              << "       --nb_threads <number of threads used to populate the DB> " << std::endl;
}


// Helper to generate a random string of given length
std::string generateRandomString(size_t length) {

    const std::string characters = "abcdefghijklmnopqrstuvwxyz"\
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device rd; // Seed
    std::mt19937 generator(rd()); // Mersenne Twister random number generator
    std::uniform_int_distribution<> distribution(0, characters.size() - 1);

    std::string random_str;
    for (size_t i = 0; i < length; ++i) {
        random_str += characters[distribution(generator)];
    }

    return random_str;
}


// Create a RocksDB instance
rocksdb::DB *CreateRocksDB() {

    rocksdb::Options options;

    // Set the options for optimizing the in-memory key-value store
    options.create_if_missing = true;
    options.compression = rocksdb::kNoCompression;
    options.disable_auto_compactions = true;
    options.allow_mmap_reads = true;
    options.allow_mmap_writes = true;
    options.IncreaseParallelism(1); // Probably keep one for compaction

    // Open the database
    rocksdb::DB* db;
    rocksdb::Status status = rocksdb::DB::Open(options,
                                               g_settings.db_path, &db);
    assert(status.ok());

    return db;
}


void PopulateRocksDB(rocksdb::DB *db) {

    std::vector<std::thread> threads;

    // Populate the small keys
    int skeys_per_thread = g_settings.skey_count / g_settings.nb_threads;
    int curr_skey_start = 0;
    int curr_skey_end = skeys_per_thread;
    threads.clear();

    // Populate the short keys
    for (int i = 0; i < g_settings.nb_threads; i++) {

        // If it is the last thread
        if (i == (g_settings.nb_threads - 1)) {
            curr_skey_end = g_settings.skey_count;
        }

        threads.emplace_back([&](int key_start, int key_end) {

            static thread_local std::mt19937 rng{std::random_device{}()};
            std::uniform_int_distribution<int> dist(g_settings.lo_skey_size,
                                                    g_settings.hi_skey_size);

            auto write_opts = rocksdb::WriteOptions();
            write_opts.disableWAL = true;

            std::string key;
            key.reserve(5 + 16); // "skey-" + 16 digits

            for (int k = key_start; k < key_end; k++) {
                // Construct the key string
                std::string num = std::to_string(k);
                key = "skey-";
                key.append(16 - num.length(), '0');
                key += num;

                // Write to the DB
                std::string skey_val = generateRandomString(dist(rng));
                db->Put(write_opts, key, skey_val);
            }
        }, curr_skey_start, curr_skey_end);

        curr_skey_start = curr_skey_end;
        curr_skey_end += skeys_per_thread;
    }

    for (auto &thread: threads) {
        thread.join();
    }

    // Populate the large keys
    int lkeys_per_thread = g_settings.lkey_count / g_settings.nb_threads;
    int curr_lkey_start = 0;
    int curr_lkey_end = lkeys_per_thread;
    threads.clear();

    for (int i = 0; i < g_settings.nb_threads; i++) {

        // If it is the last thread
        if (i == (g_settings.nb_threads - 1)) {
            curr_lkey_end = g_settings.lkey_count;
        }

        threads.emplace_back([&](int key_start, int key_end) {

            static thread_local std::mt19937 rng{std::random_device{}()};
            std::uniform_int_distribution<int> dist(g_settings.lo_lkey_size,
                                                    g_settings.hi_lkey_size);

            auto write_opts = rocksdb::WriteOptions();
            write_opts.disableWAL = true;

            std::string key;
            key.reserve(5 + 16); // "lkey-" + 16 digits

            for (int k = key_start; k < key_end; k++) {
                // Construct the key string
                std::string num = std::to_string(k);
                key = "lkey-";
                key.append(16 - num.length(), '0');
                key += num;

                // Write to the DB
                std::string lkey_val = generateRandomString(dist(rng));
                db->Put(write_opts, key, lkey_val);
            }
        }, curr_lkey_start, curr_lkey_end);

        curr_lkey_start = curr_lkey_end;
        curr_lkey_end += lkeys_per_thread;
    }

    for (auto &thread: threads) {
        thread.join();
    }

    // Perform a full major compaction
    rocksdb::Status status = db->CompactRange(rocksdb::CompactRangeOptions(),
                                              nullptr, nullptr);
    assert(status.ok());
}


// Verify if the keys and values were added correctly
void VerifyRocksDB(rocksdb::DB *db) {

    rocksdb::ReadOptions read_opts = {};
    std::string key;
    key.reserve(5 + 16); // "(s/l)key-" + 16 digits

    for (int i = 0; i < g_settings.skey_count; i++) {
        std::string num = std::to_string(i);
        key = "skey-";
        key.append(16 - num.length(), '0');
        key += num;

        std::string value;
        rocksdb::Status status = db->Get(read_opts, key, &value);
        assert(status.ok());
        assert(value.size() >= g_settings.lo_skey_size);
        assert(value.size() <= g_settings.hi_skey_size);
    }

    for (int i = 0; i < g_settings.lkey_count; i++) {
        std::string num = std::to_string(i);
        key = "lkey-";
        key.append(16 - num.length(), '0');
        key += num;

        std::string value;
        rocksdb::Status status = db->Get(read_opts, key, &value);
        assert(status.ok());
        assert(value.size() >= g_settings.lo_lkey_size);
        assert(value.size() <= g_settings.hi_lkey_size);
    }
}


// Parse the command line arguments
void ParseArguments(int argc, char *argv[]) {

    // Command line arguments
    static struct option long_opts[] = {
        {"db_path", required_argument, nullptr, 0},
        {"lo_skey_size", required_argument, nullptr, 0},
        {"hi_skey_size", required_argument, nullptr, 0},
        {"skey_count", required_argument, nullptr, 0},
        {"lo_lkey_size", required_argument, nullptr, 0},
        {"hi_lkey_size", required_argument, nullptr, 0},
        {"lkey_count", required_argument, nullptr, 0},
        {"nb_threads", required_argument, nullptr, 0},
        {nullptr, 0, nullptr, 0} // End of options
    };

    int opt = 0;
    int opt_idx = 0;

    // Extract the arguments
    while ((opt = getopt_long(argc, argv, "", long_opts,
                              &opt_idx)) != -1) {
        if (opt != 0) {
            std::cerr << "Unknown option\n";
            Help();
            exit(1);
        }

        std::string opt_name = long_opts[opt_idx].name;
        std::string opt_arg;
        if (optarg) {
            opt_arg = optarg;
        }

        if (opt_name == "db_path") {
            g_settings.db_path = opt_arg;
        } else if (opt_name == "lo_skey_size") {
            g_settings.lo_skey_size = std::stoi(opt_arg);
        } else if (opt_name == "hi_skey_size") {
            g_settings.hi_skey_size = std::stoi(opt_arg);
        } else if (opt_name == "skey_count") {
            g_settings.skey_count = std::stoi(opt_arg);
        } else if (opt_name == "lo_lkey_size") {
            g_settings.lo_lkey_size = std::stoi(opt_arg);
        } else if (opt_name == "hi_lkey_size") {
            g_settings.hi_lkey_size = std::stoi(opt_arg);
        } else if (opt_name == "lkey_count") {
            g_settings.lkey_count = std::stoi(opt_arg);
        } else if (opt_name == "nb_threads") {
            g_settings.nb_threads = std::stoi(opt_arg);
        }
    }

    // Validate the arguments
    if ((g_settings.db_path == "") ||
        (g_settings.lo_skey_size == 0) ||
        (g_settings.hi_skey_size == 0) ||
        (g_settings.lo_skey_size > g_settings.hi_skey_size) ||
        (g_settings.skey_count == 0) ||
        (g_settings.lo_lkey_size == 0) ||
        (g_settings.hi_lkey_size == 0) ||
        (g_settings.lo_lkey_size > g_settings.hi_lkey_size) ||
        (g_settings.lkey_count == 0) ||
        (g_settings.nb_threads == 0)) {
        Help();
        exit(1);
    }
}


int main(int argc, char *argv[]) {

    // Get the command line arguments
    ParseArguments(argc, argv);

    // Open the RocksDB database
    rocksdb::DB* db = CreateRocksDB();

    // Populate the RocksDB database
    PopulateRocksDB(db);

    // Verify the created database
#ifdef VERIFY_KEYS
    VerifyRocksDB(db);
#endif

    delete db;

    return 0;
}
