#include "kvdb/engine.hpp"
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

bool hex_nibble(char c, std::uint8_t& out) {
    if (c >= '0' && c <= '9') {
        out = static_cast<std::uint8_t>(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        out = static_cast<std::uint8_t>(10 + (c - 'a'));
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        out = static_cast<std::uint8_t>(10 + (c - 'A'));
        return true;
    }
    return false;
}

bool decode_hex(const std::string& hex, std::string& out) {
    if (hex == "-") {
        out.clear();
        return true;
    }
    if (hex.size() % 2 != 0) {
        return false;
    }
    out.resize(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::uint8_t hi = 0;
        std::uint8_t lo = 0;
        if (!hex_nibble(hex[2 * i], hi) || !hex_nibble(hex[2 * i + 1], lo)) {
            return false;
        }
        out[i] = static_cast<char>((hi << 4) | lo);
    }
    return true;
}

// Decodes either `hex:<hexbytes>` or raw string (with `-` treated as empty in serve mode)
std::string parse_token_bytes(const std::string& token, bool serve_mode) {
    if (serve_mode && token == "-") {
        return std::string();
    }
    if (token.rfind("hex:", 0) == 0) {
        std::string decoded;
        if (decode_hex(token.substr(4), decoded)) {
            return decoded;
        }
    }
    if (serve_mode) {
        // In serve mode, if the token is valid even-length hex and uses our protocol, decode it;
        // otherwise accept plain ASCII tokens sent manually by a judge (Section 11, Minutes 6-8).
        std::string decoded;
        if (token.size() >= 2 && decode_hex(token, decoded)) {
            return decoded;
        }
    }
    return token;
}

bool parse_batch_line(const std::string& raw_line, bool serve_mode, kvdb::BatchOp& out_op) {
    std::istringstream iss(raw_line);
    std::string cmd;
    if (!(iss >> cmd)) {
        return false;
    }
    if (cmd == "put" || cmd == "PUT") {
        std::string k_tok;
        std::string v_tok;
        if (!(iss >> k_tok)) {
            return false;
        }
        if (!(iss >> v_tok)) {
            v_tok = "";
        }
        out_op = kvdb::BatchOp::Put(
            parse_token_bytes(k_tok, serve_mode),
            parse_token_bytes(v_tok, serve_mode));
        return true;
    }
    if (cmd == "del" || cmd == "DEL" || cmd == "delete" || cmd == "DELETE") {
        std::string k_tok;
        if (!(iss >> k_tok)) {
            return false;
        }
        out_op = kvdb::BatchOp::Del(parse_token_bytes(k_tok, serve_mode));
        return true;
    }
    return false;
}

struct ServeRequest {
    enum class Kind { PUT, DEL, BATCH, COMPACT, GET } kind = Kind::PUT;
    std::string                  op_id;
    std::string                  key;
    std::string                  value;
    std::vector<kvdb::BatchOp>   batch_ops;
};

// Ensures ACK lines are printed in exact database commit order while workers run concurrently
class AckSequencer {
public:
    void emit_ordered(std::uint64_t commit_seq, const std::string& line) {
        std::unique_lock<std::mutex> lock(mu_);
        if (commit_seq == 0) {
            std::cout << line << "\n" << std::flush;
            return;
        }
        pending_[commit_seq] = line;
        while (true) {
            auto it = pending_.find(next_seq_);
            if (it == pending_.end()) {
                break;
            }
            std::cout << it->second << "\n" << std::flush;
            pending_.erase(it);
            ++next_seq_;
        }
    }

    void emit_immediate(const std::string& line) {
        std::unique_lock<std::mutex> lock(mu_);
        std::cout << line << "\n" << std::flush;
    }

private:
    std::mutex                             mu_;
    std::uint64_t                          next_seq_ = 1;
    std::map<std::uint64_t, std::string>   pending_;
};

int run_serve_loop(kvdb::Database& db) {
    std::mutex              q_mu;
    std::condition_variable q_cv;
    std::queue<ServeRequest> work_q;
    bool                    eof_reached = false;
    AckSequencer            sequencer;

    auto worker_fn = [&]() {
        while (true) {
            ServeRequest req;
            {
                std::unique_lock<std::mutex> lock(q_mu);
                q_cv.wait(lock, [&]() { return eof_reached || !work_q.empty(); });
                if (work_q.empty()) {
                    return;
                }
                req = std::move(work_q.front());
                work_q.pop();
            }

            std::uint64_t seq = 0;
            kvdb::Status st;

            switch (req.kind) {
                case ServeRequest::Kind::PUT:
                    st = db.put(req.key, req.value, &seq);
                    if (st.ok()) {
                        sequencer.emit_ordered(seq, "ACK " + req.op_id);
                    } else {
                        sequencer.emit_immediate("ERR " + req.op_id + " " + st.code_string());
                    }
                    break;

                case ServeRequest::Kind::DEL:
                    st = db.del(req.key, &seq);
                    if (st.ok()) {
                        sequencer.emit_ordered(seq, "ACK " + req.op_id);
                    } else {
                        sequencer.emit_immediate("ERR " + req.op_id + " " + st.code_string());
                    }
                    break;

                case ServeRequest::Kind::BATCH:
                    st = db.batch(req.batch_ops, &seq);
                    if (st.ok()) {
                        sequencer.emit_ordered(seq, "ACK " + req.op_id);
                    } else {
                        sequencer.emit_immediate("ERR " + req.op_id + " " + st.code_string());
                    }
                    break;

                case ServeRequest::Kind::COMPACT:
                    st = db.compact();
                    if (st.ok()) {
                        sequencer.emit_immediate("ACK " + req.op_id);
                    } else {
                        sequencer.emit_immediate("ERR " + req.op_id + " " + st.code_string());
                    }
                    break;

                case ServeRequest::Kind::GET: {
                    std::string val;
                    st = db.get(req.key, val);
                    if (st.ok()) {
                        sequencer.emit_immediate("VAL " + req.op_id + " " + val);
                    } else if (st.is_not_found()) {
                        sequencer.emit_immediate("NOTFOUND " + req.op_id);
                    } else {
                        sequencer.emit_immediate("ERR " + req.op_id + " " + st.code_string());
                    }
                    break;
                }
            }
        }
    };

    // Spawn 4 worker threads so concurrent requests hit Database::put/del/batch/get simultaneously
    std::vector<std::thread> workers;
    workers.reserve(4);
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back(worker_fn);
    }

    std::string line;
    std::uint64_t auto_id = 1;

    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::istringstream iss(line);
        std::string cmd;
        if (!(iss >> cmd)) {
            continue;
        }

        if (cmd == "QUIT" || cmd == "quit" || cmd == "EXIT" || cmd == "exit") {
            break;
        }

        if (cmd == "PUT" || cmd == "put") {
            std::string t1, t2, t3;
            iss >> t1 >> t2;
            bool has_t3 = static_cast<bool>(iss >> t3);

            ServeRequest req;
            req.kind = ServeRequest::Kind::PUT;
            if (has_t3) {
                req.op_id = t1;
                req.key   = parse_token_bytes(t2, true);
                req.value = parse_token_bytes(t3, true);
            } else {
                // Interactive manual mode: `put <key> <value>` without explicit op-id
                req.op_id = std::to_string(auto_id++);
                req.key   = parse_token_bytes(t1, false);
                req.value = parse_token_bytes(t2, false);
            }
            {
                std::lock_guard<std::mutex> lock(q_mu);
                work_q.push(std::move(req));
            }
            q_cv.notify_one();
        } else if (cmd == "DEL" || cmd == "del" || cmd == "DELETE" || cmd == "delete") {
            std::string t1, t2;
            iss >> t1;
            bool has_t2 = static_cast<bool>(iss >> t2);

            ServeRequest req;
            req.kind = ServeRequest::Kind::DEL;
            if (has_t2) {
                req.op_id = t1;
                req.key   = parse_token_bytes(t2, true);
            } else {
                req.op_id = std::to_string(auto_id++);
                req.key   = parse_token_bytes(t1, false);
            }
            {
                std::lock_guard<std::mutex> lock(q_mu);
                work_q.push(std::move(req));
            }
            q_cv.notify_one();
        } else if (cmd == "BATCH" || cmd == "batch") {
            std::string t1, t2;
            iss >> t1;
            bool has_t2 = static_cast<bool>(iss >> t2);

            ServeRequest req;
            req.kind = ServeRequest::Kind::BATCH;
            std::size_t count = 0;

            if (has_t2) {
                req.op_id = t1;
                count     = static_cast<std::size_t>(std::strtoull(t2.c_str(), nullptr, 10));
            } else {
                req.op_id = std::to_string(auto_id++);
                count     = static_cast<std::size_t>(std::strtoull(t1.c_str(), nullptr, 10));
            }

            req.batch_ops.reserve(count);
            bool frame_complete = true;
            for (std::size_t i = 0; i < count; ++i) {
                std::string mline;
                if (!std::getline(std::cin, mline)) {
                    frame_complete = false;
                    break;
                }
                if (!mline.empty() && mline.back() == '\r') {
                    mline.pop_back();
                }
                kvdb::BatchOp bop;
                if (parse_batch_line(mline, true, bop)) {
                    req.batch_ops.push_back(std::move(bop));
                }
            }
            if (frame_complete && req.batch_ops.size() == count) {
                std::lock_guard<std::mutex> lock(q_mu);
                work_q.push(std::move(req));
                q_cv.notify_one();
            }
        } else if (cmd == "COMPACT" || cmd == "compact") {
            std::string op_id;
            if (!(iss >> op_id)) {
                op_id = std::to_string(auto_id++);
            }
            ServeRequest req;
            req.kind  = ServeRequest::Kind::COMPACT;
            req.op_id = op_id;
            {
                std::lock_guard<std::mutex> lock(q_mu);
                work_q.push(std::move(req));
            }
            q_cv.notify_one();
        } else if (cmd == "GET" || cmd == "get") {
            std::string t1, t2;
            iss >> t1;
            bool has_t2 = static_cast<bool>(iss >> t2);
            ServeRequest req;
            req.kind  = ServeRequest::Kind::GET;
            req.op_id = has_t2 ? t1 : std::to_string(auto_id++);
            req.key   = parse_token_bytes(has_t2 ? t2 : t1, has_t2);
            {
                std::lock_guard<std::mutex> lock(q_mu);
                work_q.push(std::move(req));
            }
            q_cv.notify_one();
        }
    }

    {
        std::lock_guard<std::mutex> lock(q_mu);
        eof_reached = true;
    }
    q_cv.notify_all();
    for (auto& w : workers) {
        if (w.joinable()) {
            w.join();
        }
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    std::ios::sync_with_stdio(false);

    if (argc < 3) {
        std::cerr << "Usage:\n"
                  << "  kvdb open    <path> [--max-bytes N]\n"
                  << "  kvdb put     <path> <key> <value> [--max-bytes N]\n"
                  << "  kvdb get     <path> <key>\n"
                  << "  kvdb del     <path> <key> [--max-bytes N]\n"
                  << "  kvdb batch   <path> <file> [--max-bytes N]\n"
                  << "  kvdb dump    <path>\n"
                  << "  kvdb check   <path>\n"
                  << "  kvdb serve   <path> [--max-bytes N]\n"
                  << "  kvdb compact <path> [--max-bytes N]\n";
        return 1;
    }

    std::string cmd  = argv[1];
    std::string path = argv[2];

    kvdb::OpenOptions opts{};
    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--max-bytes" && i + 1 < argc) {
            opts.max_bytes = std::strtoull(argv[++i], nullptr, 10);
        }
    }

    if (cmd == "check") {
        kvdb::Status st = kvdb::Database::check(path);
        if (st.ok()) {
            std::cout << "OK\n";
            return 0;
        }
        std::cout << st.to_string() << "\n";
        return 1;
    }

    kvdb::Database db;
    kvdb::Status open_st = db.open(path, opts);
    if (!open_st.ok()) {
        std::cerr << open_st.to_string() << "\n";
        return 1;
    }

    if (cmd == "open") {
        std::cout << "keys=" << db.size()
                  << " recovery_ms=" << std::fixed << std::setprecision(2)
                  << db.last_recovery_ms() << "\n";
        db.close();
        return 0;
    }

    if (cmd == "put") {
        if (argc < 5) {
            std::cerr << "Usage: kvdb put <path> <key> <value>\n";
            return 1;
        }
        std::string key   = parse_token_bytes(argv[3], false);
        std::string value = parse_token_bytes(argv[4], false);
        kvdb::Status st   = db.put(key, value);
        db.close();
        if (!st.ok()) {
            std::cerr << st.to_string() << "\n";
            return 1;
        }
        return 0;
    }

    if (cmd == "get") {
        if (argc < 4) {
            std::cerr << "Usage: kvdb get <path> <key>\n";
            return 1;
        }
        std::string key = parse_token_bytes(argv[3], false);
        std::string val;
        kvdb::Status st = db.get(key, val);
        db.close();
        if (st.is_not_found()) {
            std::cout << "NOTFOUND\n";
            return 0;
        }
        if (!st.ok()) {
            std::cerr << st.to_string() << "\n";
            return 1;
        }
        std::cout << val << "\n";
        return 0;
    }

    if (cmd == "del" || cmd == "delete") {
        if (argc < 4) {
            std::cerr << "Usage: kvdb del <path> <key>\n";
            return 1;
        }
        std::string key = parse_token_bytes(argv[3], false);
        kvdb::Status st = db.del(key);
        db.close();
        if (!st.ok()) {
            std::cerr << st.to_string() << "\n";
            return 1;
        }
        return 0;
    }

    if (cmd == "batch") {
        if (argc < 4) {
            std::cerr << "Usage: kvdb batch <path> <file>\n";
            return 1;
        }
        std::ifstream fin(argv[3]);
        if (!fin.is_open()) {
            std::cerr << "IO_ERROR: Cannot open batch file " << argv[3] << "\n";
            return 1;
        }
        std::vector<kvdb::BatchOp> ops;
        std::string line;
        while (std::getline(fin, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            kvdb::BatchOp bop;
            if (!parse_batch_line(line, false, bop)) {
                std::cerr << "INVALID_ARGUMENT: Malformed batch line: " << line << "\n";
                return 1;
            }
            ops.push_back(std::move(bop));
        }
        kvdb::Status st = db.batch(ops);
        db.close();
        if (!st.ok()) {
            std::cerr << st.to_string() << "\n";
            return 1;
        }
        return 0;
    }

    if (cmd == "dump") {
        std::vector<std::pair<std::string, std::string>> items;
        kvdb::Status st = db.dump(items);
        db.close();
        if (!st.ok()) {
            std::cerr << st.to_string() << "\n";
            return 1;
        }
        for (const auto& kv : items) {
            std::cout << kv.first << " " << kv.second << "\n";
        }
        return 0;
    }

    if (cmd == "compact") {
        kvdb::Status st = db.compact();
        db.close();
        if (!st.ok()) {
            std::cerr << st.to_string() << "\n";
            return 1;
        }
        return 0;
    }

    if (cmd == "serve") {
        int rc = run_serve_loop(db);
        db.close();
        return rc;
    }

    std::cerr << "INVALID_ARGUMENT: Unknown command '" << cmd << "'\n";
    return 1;
}