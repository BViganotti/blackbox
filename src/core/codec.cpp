#include <blackbox/codec.hpp>

#include <array>
#include <cstring>

namespace bb {
namespace {

constexpr std::uint8_t kRecordCodecVersion = 1;

bool encode_text(Encoder& encoder, StringTable* table, std::string_view text) {
  if (table != nullptr) {
    const std::uint32_t id = table->intern(text);
    if (id == StringTable::kInvalidId) {
      return false;
    }
    encoder.varint(id);
  } else {
    encoder.raw_string(text);
  }
  return encoder.ok();
}

bool decode_text(Decoder& decoder, const StringTable* table, char* out,
                 std::size_t cap) {
  if (cap == 0) {
    return false;
  }
  out[0] = '\0';
  if (table != nullptr) {
    const std::uint64_t raw_id = decoder.varint();
    if (!decoder.ok() || raw_id > StringTable::kInvalidId - 1) {
      return false;
    }
    std::string_view text;
    if (!table->get(static_cast<std::uint32_t>(raw_id), text) ||
        text.size() >= cap) {
      return false;
    }
    if (!text.empty()) {
      std::memcpy(out, text.data(), text.size());
    }
    out[text.size()] = '\0';
    return true;
  }
  std::size_t length = 0;
  return decoder.string_into(out, cap, length);
}

void encode_value(Encoder& encoder, const StoredValue& value, StringTable* table) {
  encoder.u8(static_cast<std::uint8_t>(value.type));
  switch (value.type) {
    case ValueType::Null:
      break;
    case ValueType::Bool:
      encoder.u8(value.boolean ? 1 : 0);
      break;
    case ValueType::Int:
      encoder.ivarint(value.integer);
      break;
    case ValueType::Float:
      encoder.f64(value.real);
      break;
    case ValueType::String:
      encode_text(encoder, table, stored_text(value));
      break;
  }
}

bool decode_value(Decoder& decoder, const StringTable* table, StoredValue& out) {
  StoredValue value;
  const std::uint8_t raw_type = decoder.u8();
  if (!decoder.ok() || raw_type > static_cast<std::uint8_t>(ValueType::String)) {
    return false;
  }
  value.type = static_cast<ValueType>(raw_type);
  switch (value.type) {
    case ValueType::Null:
      break;
    case ValueType::Bool: {
      const std::uint8_t raw = decoder.u8();
      if (!decoder.ok() || raw > 1) {
        return false;
      }
      value.boolean = raw != 0;
      break;
    }
    case ValueType::Int:
      value.integer = decoder.ivarint();
      break;
    case ValueType::Float:
      value.real = decoder.f64();
      break;
    case ValueType::String: {
      std::size_t length = 0;
      if (!decode_text(decoder, table, value.text, sizeof(value.text))) {
        return false;
      }
      if (length > kMaxAttributeTextBytes) {
        return false;
      }
      // decode_text does not report the length for table-backed strings, so
      // derive it from the NUL terminator that it always writes.
      length = std::strlen(value.text);
      if (length > kMaxAttributeTextBytes) {
        return false;
      }
      value.text_length = static_cast<std::uint16_t>(length);
      break;
    }
  }
  if (!decoder.ok()) {
    return false;
  }
  out = value;
  return true;
}

}  // namespace

bool encode_record(const Record& record, Encoder& encoder, StringTable* table) {
  if (!validate_record(record)) {
    return false;
  }
  encoder.u8(kRecordCodecVersion);
  encoder.u8(static_cast<std::uint8_t>(record.kind));
  encoder.u8(static_cast<std::uint8_t>(record.level));
  encoder.u8(static_cast<std::uint8_t>(record.status));
  encoder.u8(record.attr_count);
  encoder.varint(record.seq);
  encoder.ivarint(record.wall_ns);
  encoder.ivarint(record.mono_ns);
  encoder.ivarint(record.duration_ns);
  encoder.ivarint(record.int_value);
  encoder.f64(record.double_value);
  encoder.u64(record.trace.high);
  encoder.u64(record.trace.low);
  encoder.varint(record.span);
  encoder.varint(record.parent);
  encoder.varint(record.correlation);
  encoder.varint(record.cause_seq);
  if (!encode_text(encoder, table, record_name(record)) ||
      !encode_text(encoder, table, record_message(record)) ||
      !encode_text(encoder, table, record_state_key(record))) {
    return false;
  }
  encode_value(encoder, record.value, table);
  encode_value(encoder, record.state_old, table);
  encode_value(encoder, record.state_new, table);
  for (std::size_t i = 0; i < record.attr_count; ++i) {
    const StoredAttribute& attr = record.attrs[i];
    if (!encode_text(encoder, table, stored_key(attr))) {
      return false;
    }
    encode_value(encoder, attr.value, table);
  }
  return encoder.ok();
}

bool decode_record(Decoder& decoder, Record& out, const StringTable* table) {
  Record record{};
  const std::uint8_t version = decoder.u8();
  if (!decoder.ok() || version != kRecordCodecVersion) {
    return false;
  }
  const std::uint8_t raw_kind = decoder.u8();
  const std::uint8_t raw_level = decoder.u8();
  const std::uint8_t raw_status = decoder.u8();
  const std::uint8_t attr_count = decoder.u8();
  if (!decoder.ok() || attr_count > kMaxAttributesPerRecord ||
      raw_kind < static_cast<std::uint8_t>(RecordKind::Log) ||
      raw_kind > static_cast<std::uint8_t>(RecordKind::Note) ||
      raw_level > static_cast<std::uint8_t>(Level::Fatal) ||
      raw_status > static_cast<std::uint8_t>(Status::Rejected)) {
    return false;
  }
  record.kind = static_cast<RecordKind>(raw_kind);
  record.level = static_cast<Level>(raw_level);
  record.status = static_cast<Status>(raw_status);
  record.attr_count = attr_count;
  record.seq = decoder.varint();
  record.wall_ns = decoder.ivarint();
  record.mono_ns = decoder.ivarint();
  record.duration_ns = decoder.ivarint();
  record.int_value = decoder.ivarint();
  record.double_value = decoder.f64();
  record.trace.high = decoder.u64();
  record.trace.low = decoder.u64();
  record.span = decoder.varint();
  record.parent = decoder.varint();
  record.correlation = decoder.varint();
  record.cause_seq = decoder.varint();
  if (!decoder.ok() ||
      !decode_text(decoder, table, record.name, sizeof(record.name)) ||
      !decode_text(decoder, table, record.message, sizeof(record.message)) ||
      !decode_text(decoder, table, record.state_key, sizeof(record.state_key))) {
    return false;
  }
  if (!decode_value(decoder, table, record.value) ||
      !decode_value(decoder, table, record.state_old) ||
      !decode_value(decoder, table, record.state_new)) {
    return false;
  }
  for (std::size_t i = 0; i < attr_count; ++i) {
    if (!decode_text(decoder, table, record.attrs[i].key,
                     sizeof(record.attrs[i].key)) ||
        !decode_value(decoder, table, record.attrs[i].value)) {
      return false;
    }
  }
  if (!decoder.ok() || !validate_record(record)) {
    return false;
  }
  out = record;
  return true;
}

bool encode_record_to(const Record& record, std::span<std::byte> buffer,
                      StringTable* table, std::size_t& written) {
  written = 0;
  Encoder encoder(buffer);
  if (!encode_record(record, encoder, table)) {
    return false;
  }
  written = encoder.size();
  return true;
}

bool decode_record_from(std::span<const std::byte> bytes, Record& out,
                        const StringTable* table) {
  Decoder decoder(bytes);
  return decode_record(decoder, out, table);
}

}  // namespace bb
