#pragma once

/* Rant message bytes to CBOR and back, by walking a schema's flat field table. This walk
 * moves into the Rant library later (the shared value walker); keep it contained here. */

#include "cbor.hpp"

#include "rant.hpp"

#include <string>
#include <vector>

namespace bridge {

class Codec {
public:
    Codec() = default;
    explicit Codec(const rant::Schema& s);

    bool typed() const { return !schema_.empty(); }
    const rant::Schema& schema() const { return schema_; }
    const std::vector<rant::Schema::Field>& rows() const { return rows_; }
    const std::vector<uint16_t>& kids(int row) const { return row < 0 ? roots_ : kids_.at((size_t)row); }

    /* One CBOR item for msg. skip is a field written empty, because a video track carries it. */
    bool encode(rant::Bytes msg, cbor::Writer& w, int skip = -1) const;
    /* Message bytes from one CBOR item. err names the field path on a refusal. */
    bool decode(const cbor::Item& v, std::vector<uint8_t>& out, std::string& err) const;

    /* Dotted path of a row, struct array members as "boxes[].min". */
    std::string path(int row) const;

private:
    struct Ctx;
    bool encode_row(Ctx& c, uint16_t i) const;
    bool encode_members(Ctx& c, const std::vector<uint16_t>& members) const;
    bool decode_row(rant::MessageBuilder& b, const cbor::Item& v, uint16_t i, const std::string& path, std::string& err) const;
    bool decode_members(rant::MessageBuilder& b, const cbor::Item& v, const std::vector<uint16_t>& members,
                        const std::string& prefix, std::string& err) const;

    rant::Schema schema_;
    std::vector<rant::Schema::Field> rows_;
    std::vector<std::vector<uint16_t>> kids_;
    std::vector<uint16_t> roots_;
    std::vector<int> parent_;
};

}
