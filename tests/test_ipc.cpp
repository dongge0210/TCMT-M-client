#include <gtest/gtest.h>
#include "core/IPC/IPCData.h"
#include <cstring>

using namespace tcmt::ipc;

// ── SchemaHeader layout ──
TEST(IPCDataTest, SchemaHeaderSize) {
    EXPECT_EQ(sizeof(SchemaHeader), IPC_SCHEMA_HEADER_SIZE);
    EXPECT_EQ(IPC_SCHEMA_HEADER_SIZE, 16u);
}

TEST(IPCDataTest, SchemaHeaderMagic) {
    SchemaHeader h;
    EXPECT_EQ(h.magic, IPC_MAGIC);
    EXPECT_EQ(IPC_MAGIC, 0x54434D54u); // "TCMT"
}

TEST(IPCDataTest, SchemaHeaderVersion) {
    SchemaHeader h;
    EXPECT_EQ(h.version, IPC_VERSION);
    EXPECT_EQ(IPC_VERSION, 1);
}

TEST(IPCDataTest, SchemaHeaderDefaults) {
    SchemaHeader h;
    EXPECT_EQ(h.flags, 0);
    EXPECT_EQ(h.fieldCount, 0);
    EXPECT_EQ(h.totalSize, 0);
    EXPECT_EQ(h.stringBlockSize, 0);
}

// ── FieldDef layout ──
TEST(IPCDataTest, FieldDefSize) {
    EXPECT_EQ(sizeof(FieldDef), IPC_FIELD_DEF_SIZE);
    EXPECT_EQ(IPC_FIELD_DEF_SIZE, 80u);
}

TEST(IPCDataTest, FieldDefNameLength) {
    EXPECT_EQ(sizeof(FieldDef::name), IPC_FIELD_NAME_LEN);
    EXPECT_EQ(IPC_FIELD_NAME_LEN, 32u);
}

TEST(IPCDataTest, FieldDefUnitsLength) {
    EXPECT_EQ(sizeof(FieldDef::units), IPC_FIELD_UNITS_LEN);
    EXPECT_EQ(IPC_FIELD_UNITS_LEN, 16u);
}

TEST(IPCDataTest, FieldDefDefaults) {
    FieldDef f;
    EXPECT_EQ(f.id, 0);
    EXPECT_EQ(f.type, 0);
    EXPECT_EQ(f.size, 0);
    EXPECT_EQ(f.offset, 0u);
    EXPECT_EQ(f.count, 0u);
    EXPECT_FLOAT_EQ(f.minVal, 0.0f);
    EXPECT_FLOAT_EQ(f.maxVal, 0.0f);
    EXPECT_EQ(f.name[0], '\0');
    EXPECT_EQ(f.units[0], '\0');
}

// ── Field population ──
TEST(IPCDataTest, PopulateField) {
    FieldDef f;
    f.id = 1;
    f.type = static_cast<uint8_t>(FieldType::UInt32);
    f.size = 4;
    f.offset = 128;
    f.count = 1;
    std::strncpy(f.name, "cpu/cores/physical", sizeof(f.name) - 1);
    std::strncpy(f.units, "cores", sizeof(f.units) - 1);
    f.minVal = 0.0f;
    f.maxVal = 256.0f;

    EXPECT_EQ(f.id, 1u);
    EXPECT_EQ(f.type, static_cast<uint8_t>(FieldType::UInt32));
    EXPECT_EQ(f.size, 4);
    EXPECT_EQ(f.offset, 128u);
    EXPECT_EQ(f.count, 1u);
    EXPECT_STREQ(f.name, "cpu/cores/physical");
    EXPECT_STREQ(f.units, "cores");
    EXPECT_FLOAT_EQ(f.minVal, 0.0f);
    EXPECT_FLOAT_EQ(f.maxVal, 256.0f);
}

TEST(IPCDataTest, FieldTypes) {
    EXPECT_EQ(static_cast<int>(FieldType::UInt8), 1);
    EXPECT_EQ(static_cast<int>(FieldType::Int8), 2);
    EXPECT_EQ(static_cast<int>(FieldType::UInt16), 3);
    EXPECT_EQ(static_cast<int>(FieldType::Int16), 4);
    EXPECT_EQ(static_cast<int>(FieldType::UInt32), 5);
    EXPECT_EQ(static_cast<int>(FieldType::Int32), 6);
    EXPECT_EQ(static_cast<int>(FieldType::UInt64), 7);
    EXPECT_EQ(static_cast<int>(FieldType::Int64), 8);
    EXPECT_EQ(static_cast<int>(FieldType::Float32), 9);
    EXPECT_EQ(static_cast<int>(FieldType::Float64), 10);
    EXPECT_EQ(static_cast<int>(FieldType::Bool), 11);
    EXPECT_EQ(static_cast<int>(FieldType::String), 12);
    EXPECT_EQ(static_cast<int>(FieldType::WString), 13);
}

// ── Serialization: simulate NamedPipeServer::SerializeSchema() ──
TEST(IPCDataTest, SerializeSingleField) {
    SchemaHeader header;
    header.fieldCount = 1;
    header.totalSize = IPC_SCHEMA_HEADER_SIZE + IPC_FIELD_DEF_SIZE;

    std::vector<FieldDef> fields(1);
    fields[0].id = 1;
    fields[0].type = static_cast<uint8_t>(FieldType::UInt32);
    fields[0].size = 4;
    fields[0].offset = 0;
    fields[0].count = 1;
    std::strncpy(fields[0].name, "test/field", sizeof(fields[0].name) - 1);

    // Serialize
    std::vector<uint8_t> buf;
    buf.resize(IPC_SCHEMA_HEADER_SIZE + fields.size() * IPC_FIELD_DEF_SIZE);
    std::memcpy(buf.data(), &header, IPC_SCHEMA_HEADER_SIZE);
    for (size_t i = 0; i < fields.size(); ++i) {
        std::memcpy(buf.data() + IPC_SCHEMA_HEADER_SIZE + i * IPC_FIELD_DEF_SIZE,
                    &fields[i], IPC_FIELD_DEF_SIZE);
    }

    // Deserialize and verify
    SchemaHeader out_header;
    std::memcpy(&out_header, buf.data(), IPC_SCHEMA_HEADER_SIZE);
    EXPECT_EQ(out_header.magic, IPC_MAGIC);
    EXPECT_EQ(out_header.fieldCount, 1);
    EXPECT_EQ(out_header.totalSize, IPC_SCHEMA_HEADER_SIZE + IPC_FIELD_DEF_SIZE);

    FieldDef out_field;
    std::memcpy(&out_field, buf.data() + IPC_SCHEMA_HEADER_SIZE, IPC_FIELD_DEF_SIZE);
    EXPECT_EQ(out_field.id, 1u);
    EXPECT_EQ(out_field.type, static_cast<uint8_t>(FieldType::UInt32));
    EXPECT_STREQ(out_field.name, "test/field");
}

TEST(IPCDataTest, SerializeMultipleFields) {
    constexpr uint16_t N = 3;
    SchemaHeader header;
    header.fieldCount = N;
    header.totalSize = IPC_SCHEMA_HEADER_SIZE + N * IPC_FIELD_DEF_SIZE;

    std::vector<FieldDef> fields(N);
    const char* names[N] = {"cpu/usage", "memory/total", "gpu/0/name"};
    for (uint16_t i = 0; i < N; ++i) {
        fields[i].id = i + 1;
        fields[i].type = static_cast<uint8_t>(FieldType::Float64);
        fields[i].size = 8;
        fields[i].offset = i * 8;
        fields[i].count = 1;
        std::strncpy(fields[i].name, names[i], sizeof(fields[i].name) - 1);
    }

    // Serialize
    std::vector<uint8_t> buf;
    buf.resize(IPC_SCHEMA_HEADER_SIZE + fields.size() * IPC_FIELD_DEF_SIZE);
    std::memcpy(buf.data(), &header, IPC_SCHEMA_HEADER_SIZE);
    for (size_t i = 0; i < fields.size(); ++i) {
        std::memcpy(buf.data() + IPC_SCHEMA_HEADER_SIZE + i * IPC_FIELD_DEF_SIZE,
                    &fields[i], IPC_FIELD_DEF_SIZE);
    }

    // Deserialize
    SchemaHeader out_header;
    std::memcpy(&out_header, buf.data(), IPC_SCHEMA_HEADER_SIZE);
    EXPECT_EQ(out_header.fieldCount, N);

    for (uint16_t i = 0; i < N; ++i) {
        FieldDef f;
        std::memcpy(&f, buf.data() + IPC_SCHEMA_HEADER_SIZE + i * IPC_FIELD_DEF_SIZE,
                     IPC_FIELD_DEF_SIZE);
        EXPECT_EQ(f.id, i + 1);
        EXPECT_STREQ(f.name, names[i]);
    }
}

TEST(IPCDataTest, MaxFields) {
    SchemaHeader header;
    header.fieldCount = IPC_MAX_FIELDS;
    header.totalSize = IPC_SCHEMA_HEADER_SIZE + IPC_MAX_FIELDS * IPC_FIELD_DEF_SIZE;

    std::vector<FieldDef> fields(IPC_MAX_FIELDS);
    for (uint16_t i = 0; i < IPC_MAX_FIELDS; ++i) {
        fields[i].id = i + 1;
        fields[i].type = static_cast<uint8_t>(FieldType::UInt8);
        fields[i].size = 1;
        fields[i].offset = i;
    }

    std::vector<uint8_t> buf;
    buf.resize(IPC_SCHEMA_HEADER_SIZE + fields.size() * IPC_FIELD_DEF_SIZE);
    std::memcpy(buf.data(), &header, IPC_SCHEMA_HEADER_SIZE);
    for (size_t i = 0; i < fields.size(); ++i) {
        std::memcpy(buf.data() + IPC_SCHEMA_HEADER_SIZE + i * IPC_FIELD_DEF_SIZE,
                    &fields[i], IPC_FIELD_DEF_SIZE);
    }

    SchemaHeader out_header;
    std::memcpy(&out_header, buf.data(), IPC_SCHEMA_HEADER_SIZE);
    EXPECT_EQ(out_header.fieldCount, IPC_MAX_FIELDS);

    FieldDef last;
    std::memcpy(&last, buf.data() + IPC_SCHEMA_HEADER_SIZE + (IPC_MAX_FIELDS - 1) * IPC_FIELD_DEF_SIZE,
                 IPC_FIELD_DEF_SIZE);
    EXPECT_EQ(last.id, IPC_MAX_FIELDS);

    // Total buffer size
    EXPECT_EQ(buf.size(), IPC_SCHEMA_HEADER_SIZE + IPC_MAX_FIELDS * IPC_FIELD_DEF_SIZE);
}

// ── Name length boundary ──
TEST(IPCDataTest, NameTruncation) {
    FieldDef f;
    std::string longName(IPC_FIELD_NAME_LEN, 'x'); // exactly len
    std::strncpy(f.name, longName.c_str(), sizeof(f.name) - 1);
    EXPECT_EQ(std::strlen(f.name), IPC_FIELD_NAME_LEN - 1); // null-terminated
}
