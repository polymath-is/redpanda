#include "storage/opfuzz/opfuzz.h"

#include "model/record.h"
#include "random/generators.h"
#include "vassert.h"
#include "vlog.h"

#include <seastar/util/backtrace.hh>

#include <memory>

namespace storage {
// NOLINTNEXTLINE
ss::logger fuzzlogger("opfuzz");

struct append_op final : opfuzz::op {
    ~append_op() noexcept override = default;
    const char* name() const final { return "append"; }
    ss::future<> invoke(opfuzz::op_context ctx) final {
        storage::log_append_config append_cfg{
          storage::log_append_config::fsync::yes,
          ss::default_priority_class(),
          model::no_timeout};
        auto batches = storage::test::make_random_batches(model::offset(0), 10);
        vlog(
          fuzzlogger.info,
          "Appending: {} batches. {}-{}",
          batches.size(),
          batches.front().base_offset(),
          batches.back().last_offset());
        for (auto& b : batches) {
            b.set_term(*ctx.term);
        }
        auto reader = model::make_memory_record_batch_reader(
          std::move(batches));

        return std::move(reader)
          .consume(ctx.log->make_appender(append_cfg), model::no_timeout)
          .discard_result();
    }
};
struct truncate_op final : opfuzz::op {
    struct collect_base_offsets {
        ss::future<ss::stop_iteration> operator()(model::record_batch batch) {
            offsets.push_back(batch.base_offset());
            return ss::make_ready_future<ss::stop_iteration>(
              ss::stop_iteration::no);
        }
        std::vector<model::offset> end_of_stream() {
            return std::move(offsets);
        }
        std::vector<model::offset> offsets;
    };

    ~truncate_op() noexcept override = default;
    const char* name() const final { return "truncate"; }
    ss::future<> invoke(opfuzz::op_context ctx) final {
        storage::log_reader_config cfg(
          model::offset(0),
          ctx.log->dirty_offset(),
          ss::default_priority_class());
        vlog(fuzzlogger.info, "collect base offsets {} - {}", cfg, *ctx.log);
        return ctx.log->make_reader(cfg)
          .then([](model::record_batch_reader reader) {
              return std::move(reader).consume(
                collect_base_offsets{}, model::no_timeout);
          })
          .then([ctx](std::vector<model::offset> ofs) {
              vlog(fuzzlogger.info, "base offsets collected: {}", ofs);
              model::offset to{0};
              if (!ofs.empty()) {
                  to = ofs[random_generators::get_int<size_t>(
                    0, ofs.size() - 1)];
              }
              vlog(fuzzlogger.info, "Truncating log at offset: {}", to);
              return ctx.log->truncate(to);
          });
    }
};

struct simple_verify_consumer {
    ss::future<ss::stop_iteration> operator()(model::record_batch b) {
        if (auto crc = model::crc_record_batch(b); b.header().crc != crc) {
            auto ptr = ss::make_backtraced_exception_ptr<std::runtime_error>(
              fmt::format(
                "Expected CRC: {}, but got CRC:{} - invalid batch: {}",
                b.header().crc,
                crc,
                b));
            return ss::make_exception_future<ss::stop_iteration>(ptr);
        }
        return ss::make_ready_future<ss::stop_iteration>(
          ss::stop_iteration::no);
    }
    void end_of_stream() {}
};

struct read_op final : opfuzz::op {
    ~read_op() noexcept override = default;
    const char* name() const final { return "read"; }
    ss::future<> invoke(opfuzz::op_context ctx) final {
        model::offset start{0};
        model::offset end{0};
        auto dirty_offset = ctx.log->dirty_offset();
        if (dirty_offset > start) {
            start = model::offset(
              random_generators::get_int<model::offset::type>(
                0, dirty_offset()));
        }
        if (start > end) {
            end = model::offset(random_generators::get_int<model::offset::type>(
              start(), dirty_offset()));
        }
        storage::log_reader_config cfg(
          start, end, ss::default_priority_class());
        vlog(fuzzlogger.info, "Read [{},{}] - {}", start, end, *ctx.log);
        return ctx.log->make_reader(cfg).then(
          [](model::record_batch_reader reader) {
              return std::move(reader).consume(
                simple_verify_consumer{}, model::no_timeout);
          });
    }
};
struct flush_op final : opfuzz::op {
    ~flush_op() noexcept override = default;
    const char* name() const final { return "flush"; }
    ss::future<> invoke(opfuzz::op_context ctx) final {
        return ctx.log->flush();
    }
};
struct term_roll_op final : opfuzz::op {
    ~term_roll_op() noexcept override = default;
    const char* name() const final { return "term_roll"; }
    ss::future<> invoke(opfuzz::op_context ctx) final {
        (*ctx.term)++;
        return ss::make_ready_future<>();
    }
};

ss::future<> opfuzz::execute() {
    // execute commands in sequence
    return ss::do_for_each(_workload, [this](std::unique_ptr<op>& c) {
        vlog(fuzzlogger.info, "Executing: {}", c->name());
        return c->invoke(op_context{&_term, &_log});
    });
}

std::unique_ptr<opfuzz::op> opfuzz::random_operation() {
    switch (random_generators::get_int(0, 4)) {
    case 0:
        return std::make_unique<append_op>();
    case 1:
        return std::make_unique<truncate_op>();
    case 2:
        return std::make_unique<read_op>();
    case 3:
        return std::make_unique<flush_op>();
    case 4:
        return std::make_unique<term_roll_op>();
    }
    vassert(false, "could not generate random operation for log");
}

void opfuzz::generate_workload(size_t count) {
    _workload.reserve(count);
    std::generate_n(std::back_inserter(_workload), count, [this] {
        return random_operation();
    });
}
} // namespace storage