/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "orc/Common.hh"

#include <sstream>

namespace orc {

  std::string compressionKindToString(CompressionKind kind) {
    switch (static_cast<int>(kind)) {
      case CompressionKind_NONE:
        return "none";
      case CompressionKind_ZLIB:
        return "zlib";
      case CompressionKind_SNAPPY:
        return "snappy";
      case CompressionKind_LZO:
        return "lzo";
      case CompressionKind_LZ4:
        return "lz4";
      case CompressionKind_ZSTD:
        return "zstd";
    }
    std::stringstream buffer;
    buffer << "unknown - " << kind;
    return buffer.str();
  }

  std::string writerVersionToString(WriterVersion version) {
    switch (static_cast<int>(version)) {
      case WriterVersion_ORIGINAL:
        return "original";
      case WriterVersion_HIVE_8732:
        return "HIVE-8732";
      case WriterVersion_HIVE_4243:
        return "HIVE-4243";
      case WriterVersion_HIVE_12055:
        return "HIVE-12055";
      case WriterVersion_HIVE_13083:
        return "HIVE-13083";
      case WriterVersion_ORC_101:
        return "ORC-101";
      case WriterVersion_ORC_135:
        return "ORC-135";
    }
    std::stringstream buffer;
    buffer << "future - " << version;
    return buffer.str();
  }

  std::string streamKindToString(StreamKind kind) {
    switch (static_cast<int>(kind)) {
      case StreamKind_PRESENT:
        return "present";
      case StreamKind_DATA:
        return "data";
      case StreamKind_LENGTH:
        return "length";
      case StreamKind_DICTIONARY_DATA:
        return "dictionary";
      case StreamKind_DICTIONARY_COUNT:
        return "dictionary count";
      case StreamKind_SECONDARY:
        return "secondary";
      case StreamKind_ROW_INDEX:
        return "index";
      case StreamKind_BLOOM_FILTER:
        return "bloom";
    }
    std::stringstream buffer;
    buffer << "unknown - " << kind;
    return buffer.str();
  }

  std::string columnEncodingKindToString(ColumnEncodingKind kind) {
    switch (static_cast<int>(kind)) {
      case ColumnEncodingKind_DIRECT:
        return "direct";
      case ColumnEncodingKind_DICTIONARY:
        return "dictionary";
      case ColumnEncodingKind_DIRECT_V2:
        return "direct rle2";
      case ColumnEncodingKind_DICTIONARY_V2:
        return "dictionary rle2";
    }
    std::stringstream buffer;
    buffer << "unknown - " << kind;
    return buffer.str();
  }

}
