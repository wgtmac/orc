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

#include "orc/Int128.hh"
#include "orc/Writer.hh"

#include "ByteRLE.hh"
#include "ColumnWriter.hh"
#include "RLE.hh"
#include "Statistics.hh"
#include "Timezone.hh"

namespace orc {
  StreamsFactory::~StreamsFactory() {
    //PASS
  }

  class StreamsFactoryImpl : public StreamsFactory {
  public:
    StreamsFactoryImpl(
                       const WriterOptions& writerOptions,
                       OutputStream* outputStream) :
                       options(writerOptions),
                       outStream(outputStream) {
                       }

    virtual std::unique_ptr<BufferedOutputStream>
                    createStream(proto::Stream_Kind kind) const override;
  private:
    const WriterOptions& options;
    OutputStream* outStream;
  };

  std::unique_ptr<BufferedOutputStream> StreamsFactoryImpl::createStream(
                                                    proto::Stream_Kind) const {
    // In the future, we can decide compression strategy and modifier
    // based on stream kind. But for now we just use the setting from
    // WriterOption
    return createCompressor(
                            options.getCompression(),
                            outStream,
                            options.getCompressionStrategy(),
                            // BufferedOutputStream initial capacity
                            1 * 1024 * 1024,
                            options.getCompressionBlockSize(),
                            *options.getMemoryPool());
  }

  std::unique_ptr<StreamsFactory> createStreamsFactory(
                                        const WriterOptions& options,
                                        OutputStream* outStream) {
    return std::unique_ptr<StreamsFactory>(
                                   new StreamsFactoryImpl(options, outStream));
  }

  RowIndexPositionRecorder::~RowIndexPositionRecorder() {
    // PASS
  }

  ColumnWriter::ColumnWriter(
                             const Type& type,
                             const StreamsFactory& factory,
                             const WriterOptions& options) :
                                columnId(type.getColumnId()),
                                colIndexStatistics(),
                                colStripeStatistics(),
                                colFileStatistics(),
                                enableIndex(options.getEnableIndex()),
                                rowIndex(),
                                rowIndexEntry(),
                                rowIndexPosition(),
                                memPool(*options.getMemoryPool()),
                                indexStream() {

    std::unique_ptr<BufferedOutputStream> presentStream =
        factory.createStream(proto::Stream_Kind_PRESENT);
    notNullEncoder = createBooleanRleEncoder(std::move(presentStream));

    colIndexStatistics = createColumnStatistics(type);
    colStripeStatistics = createColumnStatistics(type);
    colFileStatistics = createColumnStatistics(type);

    if (enableIndex) {
      rowIndex = std::unique_ptr<proto::RowIndex>(new proto::RowIndex());
      rowIndexEntry =
        std::unique_ptr<proto::RowIndexEntry>(new proto::RowIndexEntry());
      rowIndexPosition = std::unique_ptr<RowIndexPositionRecorder>(
                     new RowIndexPositionRecorder(*rowIndexEntry));
      indexStream =
        factory.createStream(proto::Stream_Kind_ROW_INDEX);
    }
  }

  ColumnWriter::~ColumnWriter() {
    // PASS
  }

  void ColumnWriter::add(ColumnVectorBatch& batch,
                         uint64_t offset,
                         uint64_t numValues) {
    notNullEncoder->add(batch.notNull.data() + offset, numValues, nullptr);
  }

  void ColumnWriter::flush(std::vector<proto::Stream>& streams) {
    proto::Stream stream;
    stream.set_kind(proto::Stream_Kind_PRESENT);
    stream.set_column(static_cast<uint32_t>(columnId));
    stream.set_length(notNullEncoder->flush());
    streams.push_back(stream);
  }

  uint64_t ColumnWriter::getEstimatedSize() const {
    return notNullEncoder->getBufferSize();
  }

  void ColumnWriter::getStripeStatistics(
    std::vector<proto::ColumnStatistics>& stats) const {
    getProtoBufStatistics(stats, colStripeStatistics.get());
  }

  void ColumnWriter::mergeStripeStatsIntoFileStats() {
    colFileStatistics->merge(*colStripeStatistics);
    colStripeStatistics->reset();
  }

  void ColumnWriter::mergeRowGroupStatsIntoStripeStats() {
    colStripeStatistics->merge(*colIndexStatistics);
    colIndexStatistics->reset();
  }

  void ColumnWriter::getFileStatistics(
    std::vector<proto::ColumnStatistics>& stats) const {
    getProtoBufStatistics(stats, colFileStatistics.get());
  }

  void ColumnWriter::createRowIndexEntry() {
    proto::ColumnStatistics *indexStats = rowIndexEntry->mutable_statistics();
    colIndexStatistics->toProtoBuf(*indexStats);

    *rowIndex->add_entry() = *rowIndexEntry;

    rowIndexEntry->clear_positions();
    rowIndexEntry->clear_statistics();

    colStripeStatistics->merge(*colIndexStatistics);
    colIndexStatistics->reset();

    recordPosition();
  }

  void ColumnWriter::writeIndex(std::vector<proto::Stream> &streams) const {
    // write row index to output stream
    rowIndex->SerializeToZeroCopyStream(indexStream.get());

    // construct row index stream
    proto::Stream stream;
    stream.set_kind(proto::Stream_Kind_ROW_INDEX);
    stream.set_column(static_cast<uint32_t>(columnId));
    stream.set_length(indexStream->flush());
    streams.push_back(stream);
  }

  void ColumnWriter::recordPosition() const {
    notNullEncoder->recordPosition(rowIndexPosition.get());
  }

  void ColumnWriter::reset() {
    if (enableIndex) {
      // clear row index
      rowIndex->clear_entry();
      rowIndexEntry->clear_positions();
      rowIndexEntry->clear_statistics();

      // write current positions
      recordPosition();
    }
  }

  class StructColumnWriter : public ColumnWriter {
  public:
    StructColumnWriter(
                       const Type& type,
                       const StreamsFactory& factory,
                       const WriterOptions& options);
    ~StructColumnWriter();

    virtual void add(ColumnVectorBatch& rowBatch,
                     uint64_t offset,
                     uint64_t numValues) override;

    virtual void flush(std::vector<proto::Stream>& streams) override;

    virtual uint64_t getEstimatedSize() const override;
    virtual void getColumnEncoding(
      std::vector<proto::ColumnEncoding>& encodings) const override;

    virtual void getStripeStatistics(
      std::vector<proto::ColumnStatistics>& stats) const override;

    virtual void getFileStatistics(
      std::vector<proto::ColumnStatistics>& stats) const override;

    virtual void mergeStripeStatsIntoFileStats() override;

    virtual void mergeRowGroupStatsIntoStripeStats() override;

    virtual void createRowIndexEntry() override;

    virtual void writeIndex(
      std::vector<proto::Stream> &streams) const override;

    virtual void reset() override;

  private:
    std::vector<ColumnWriter *> children;
  };

  StructColumnWriter::StructColumnWriter(
                                         const Type& type,
                                         const StreamsFactory& factory,
                                         const WriterOptions& options) :
                                         ColumnWriter(type, factory, options) {
    for(unsigned int i = 0; i < type.getSubtypeCount(); ++i) {
      const Type& child = *type.getSubtype(i);
      children.push_back(buildWriter(child, factory, options).release());
    }

    if (enableIndex) {
      recordPosition();
    }
  }

  StructColumnWriter::~StructColumnWriter() {
    for (uint32_t i = 0; i < children.size(); ++i) {
      delete children[i];
    }
  }

  void StructColumnWriter::add(
                              ColumnVectorBatch& rowBatch,
                              uint64_t offset,
                              uint64_t numValues) {
    ColumnWriter::add(rowBatch, offset, numValues);

    const StructVectorBatch & structBatch =
          dynamic_cast<const StructVectorBatch &>(rowBatch);

    for (uint32_t i = 0; i < children.size(); ++i) {
      children[i]->add(*structBatch.fields[i], offset, numValues);
    }

    // update stats
    bool hasNull = false;
    if (!structBatch.hasNulls) {
      colIndexStatistics->increase(numValues);
    } else {
      const char* notNull = structBatch.notNull.data() + offset;
      for (uint64_t i = 0; i < numValues; ++i) {
        if (notNull[i]) {
          colIndexStatistics->increase(1);
        } else if (!hasNull) {
          hasNull = true;
        }
      }
    }
    colIndexStatistics->setHasNull(hasNull);
  }

  void StructColumnWriter::flush(std::vector<proto::Stream>& streams) {
    ColumnWriter::flush(streams);
    for (uint32_t i = 0; i < children.size(); ++i) {
      children[i]->flush(streams);
    }
  }

  void StructColumnWriter::writeIndex(
                      std::vector<proto::Stream> &streams) const {
    ColumnWriter::writeIndex(streams);
    for (uint32_t i = 0; i < children.size(); ++i) {
      children[i]->writeIndex(streams);
    }
  }

  uint64_t StructColumnWriter::getEstimatedSize() const {
    uint64_t size = ColumnWriter::getEstimatedSize();
    for (uint32_t i = 0; i < children.size(); ++i) {
      size += children[i]->getEstimatedSize();
    }
    return size;
  }

  void StructColumnWriter::getColumnEncoding(
                      std::vector<proto::ColumnEncoding>& encodings) const {
    proto::ColumnEncoding encoding;
    encoding.set_kind(proto::ColumnEncoding_Kind_DIRECT);
    encoding.set_dictionarysize(0);
    encodings.push_back(encoding);
    for (uint32_t i = 0; i < children.size(); ++i) {
      children[i]->getColumnEncoding(encodings);
    }
  }

  void StructColumnWriter::getStripeStatistics(
    std::vector<proto::ColumnStatistics>& stats) const {
    ColumnWriter::getStripeStatistics(stats);

    for (uint32_t i = 0; i < children.size(); ++i) {
      children[i]->getStripeStatistics(stats);
    }
  }

  void StructColumnWriter::mergeStripeStatsIntoFileStats() {
    ColumnWriter::mergeStripeStatsIntoFileStats();

    for (uint32_t i = 0; i < children.size(); ++i) {
      children[i]->mergeStripeStatsIntoFileStats();
    }
  }

  void StructColumnWriter::getFileStatistics(
    std::vector<proto::ColumnStatistics>& stats) const {
    ColumnWriter::getFileStatistics(stats);

    for (uint32_t i = 0; i < children.size(); ++i) {
      children[i]->getFileStatistics(stats);
    }
  }

  void StructColumnWriter::mergeRowGroupStatsIntoStripeStats()  {
    ColumnWriter::mergeRowGroupStatsIntoStripeStats();

    for (uint32_t i = 0; i < children.size(); ++i) {
      children[i]->mergeRowGroupStatsIntoStripeStats();
    }
  }

  void StructColumnWriter::createRowIndexEntry() {
    ColumnWriter::createRowIndexEntry();

    for (uint32_t i = 0; i < children.size(); ++i) {
      children[i]->createRowIndexEntry();
    }
  }

  void StructColumnWriter::reset() {
    ColumnWriter::reset();

    for (uint32_t i = 0; i < children.size(); ++i) {
      children[i]->reset();
    }
  }

  class IntegerColumnWriter : public ColumnWriter {
  public:
    IntegerColumnWriter(
                        const Type& type,
                        const StreamsFactory& factory,
                        const WriterOptions& options);

    virtual void add(ColumnVectorBatch& rowBatch,
                     uint64_t offset,
                     uint64_t numValues) override;

    virtual void flush(std::vector<proto::Stream>& streams) override;

    virtual uint64_t getEstimatedSize() const override;

    virtual void getColumnEncoding(
              std::vector<proto::ColumnEncoding>& encodings) const override;

    virtual void recordPosition() const override;

  protected:
    std::unique_ptr<RleEncoder> rleEncoder;

  private:
    RleVersion rleVersion;
  };

  IntegerColumnWriter::IntegerColumnWriter(
                        const Type& type,
                        const StreamsFactory& factory,
                        const WriterOptions& options) :
                          ColumnWriter(type, factory, options),
                          rleVersion(RleVersion_1) {
    std::unique_ptr<BufferedOutputStream> dataStream =
      factory.createStream(proto::Stream_Kind_DATA);
    rleEncoder = createRleEncoder(
                                  std::move(dataStream),
                                  true,
                                  rleVersion,
                                  memPool);

    if (enableIndex) {
      recordPosition();
    }
  }

  void IntegerColumnWriter::add(
                              ColumnVectorBatch& rowBatch,
                              uint64_t offset,
                              uint64_t numValues) {
    ColumnWriter::add(rowBatch, offset, numValues);

    const LongVectorBatch & longBatch =
                    dynamic_cast<const LongVectorBatch &>(rowBatch);

    const int64_t* data = longBatch.data.data() + offset;
    const char* notNull = longBatch.hasNulls ?
                          longBatch.notNull.data() + offset : nullptr;

    rleEncoder->add(data, numValues, notNull);

    // update stats
    IntegerColumnStatisticsImpl* intStats =
      dynamic_cast<IntegerColumnStatisticsImpl*>(colIndexStatistics.get());
    bool hasNull = false;
    for (uint64_t i = 0; i < numValues; ++i) {
      if (notNull == nullptr || notNull[i]) {
        intStats->increase(1);
        intStats->update(data[i], 1);
      } else if (!hasNull) {
        hasNull = true;
      }
    }
    intStats->setHasNull(hasNull);
  }

  void IntegerColumnWriter::flush(std::vector<proto::Stream>& streams) {
    ColumnWriter::flush(streams);

    proto::Stream stream;
    stream.set_kind(proto::Stream_Kind_DATA);
    stream.set_column(static_cast<uint32_t>(columnId));
    stream.set_length(rleEncoder->flush());
    streams.push_back(stream);
  }

  uint64_t IntegerColumnWriter::getEstimatedSize() const {
    uint64_t size = ColumnWriter::getEstimatedSize();
    size += rleEncoder->getBufferSize();
    return size;
  }

  void IntegerColumnWriter::getColumnEncoding(
                       std::vector<proto::ColumnEncoding>& encodings) const {
    proto::ColumnEncoding encoding;
    encoding.set_kind(rleVersion == RleVersion_1 ?
                                proto::ColumnEncoding_Kind_DIRECT :
                                proto::ColumnEncoding_Kind_DIRECT_V2);
    encoding.set_dictionarysize(0);
    encodings.push_back(encoding);
  }

  void IntegerColumnWriter::recordPosition() const {
    ColumnWriter::recordPosition();
    rleEncoder->recordPosition(rowIndexPosition.get());
  }

  class ByteColumnWriter : public ColumnWriter {
  public:
    ByteColumnWriter(const Type& type,
                     const StreamsFactory& factory,
                     const WriterOptions& options);

    virtual void add(ColumnVectorBatch& rowBatch,
                     uint64_t offset,
                     uint64_t numValues) override;

    virtual void flush(std::vector<proto::Stream>& streams) override;

    virtual uint64_t getEstimatedSize() const override;

    virtual void getColumnEncoding(
            std::vector<proto::ColumnEncoding>& encodings) const override;

    virtual void recordPosition() const override;

  private:
    std::unique_ptr<ByteRleEncoder> byteRleEncoder;
  };

  ByteColumnWriter::ByteColumnWriter(
                        const Type& type,
                        const StreamsFactory& factory,
                        const WriterOptions& options) :
                             ColumnWriter(type, factory, options) {
    std::unique_ptr<BufferedOutputStream> dataStream =
                                  factory.createStream(proto::Stream_Kind_DATA);
    byteRleEncoder = createByteRleEncoder(std::move(dataStream));

    if (enableIndex) {
      recordPosition();
    }
  }

  void ByteColumnWriter::add(ColumnVectorBatch& rowBatch,
                             uint64_t offset,
                             uint64_t numValues) {
    ColumnWriter::add(rowBatch, offset, numValues);

    LongVectorBatch& byteBatch =
                               dynamic_cast<LongVectorBatch&>(rowBatch);

    int64_t* data = byteBatch.data.data() + offset;
    const char* notNull = byteBatch.hasNulls ?
                          byteBatch.notNull.data() + offset : nullptr;

    char* byteData = reinterpret_cast<char*>(data);
    for (uint64_t i = 0; i < numValues; ++i) {
      byteData[i] = static_cast<char>(data[i]);
    }
    byteRleEncoder->add(byteData, numValues, notNull);

    IntegerColumnStatisticsImpl* intStats =
        dynamic_cast<IntegerColumnStatisticsImpl*>(colIndexStatistics.get());
    bool hasNull = false;
    for (uint64_t i = 0; i < numValues; ++i) {
      if (notNull == nullptr || notNull[i]) {
        intStats->increase(1);
        intStats->update(static_cast<int64_t>(byteData[i]), 1);
      } else if (!hasNull) {
        hasNull = true;
      }
    }
    intStats->setHasNull(hasNull);
  }

  void ByteColumnWriter::flush(std::vector<proto::Stream>& streams) {
    ColumnWriter::flush(streams);

    proto::Stream stream;
    stream.set_kind(proto::Stream_Kind_DATA);
    stream.set_column(static_cast<uint32_t>(columnId));
    stream.set_length(byteRleEncoder->flush());
    streams.push_back(stream);
  }

  uint64_t ByteColumnWriter::getEstimatedSize() const {
    uint64_t size = ColumnWriter::getEstimatedSize();
    size += byteRleEncoder->getBufferSize();
    return size;
  }

  void ByteColumnWriter::getColumnEncoding(
    std::vector<proto::ColumnEncoding>& encodings) const {
    proto::ColumnEncoding encoding;
    encoding.set_kind(proto::ColumnEncoding_Kind_DIRECT);
    encoding.set_dictionarysize(0);
    encodings.push_back(encoding);
  }

  void ByteColumnWriter::recordPosition() const {
    ColumnWriter::recordPosition();
    byteRleEncoder->recordPosition(rowIndexPosition.get());
  }

  class BooleanColumnWriter : public ColumnWriter {
  public:
    BooleanColumnWriter(const Type& type,
                        const StreamsFactory& factory,
                        const WriterOptions& options);

    virtual void add(ColumnVectorBatch& rowBatch,
                     uint64_t offset,
                     uint64_t numValues) override;

    virtual void flush(std::vector<proto::Stream>& streams) override;

    virtual uint64_t getEstimatedSize() const override;

    virtual void getColumnEncoding(
        std::vector<proto::ColumnEncoding>& encodings) const override;

    virtual void recordPosition() const override;

  private:
    std::unique_ptr<ByteRleEncoder> rleEncoder;
  };

  BooleanColumnWriter::BooleanColumnWriter(
                           const Type& type,
                           const StreamsFactory& factory,
                           const WriterOptions& options) :
                               ColumnWriter(type, factory, options) {
    std::unique_ptr<BufferedOutputStream> dataStream =
      factory.createStream(proto::Stream_Kind_DATA);
    rleEncoder = createBooleanRleEncoder(std::move(dataStream));

    if (enableIndex) {
      recordPosition();
    }
  }

  void BooleanColumnWriter::add(ColumnVectorBatch& rowBatch,
                                uint64_t offset,
                                uint64_t numValues) {
    ColumnWriter::add(rowBatch, offset, numValues);

    LongVectorBatch& byteBatch = dynamic_cast<LongVectorBatch&>(rowBatch);
    int64_t* data = byteBatch.data.data() + offset;
    const char* notNull = byteBatch.hasNulls ?
                          byteBatch.notNull.data() + offset : nullptr;

    char* byteData = reinterpret_cast<char*>(data);
    for (uint64_t i = 0; i < numValues; ++i) {
      byteData[i] = static_cast<char>(data[i]);
    }
    rleEncoder->add(byteData, numValues, notNull);

    BooleanColumnStatisticsImpl* boolStats =
        dynamic_cast<BooleanColumnStatisticsImpl*>(colIndexStatistics.get());
    bool hasNull = false;
    for (uint64_t i = 0; i < numValues; ++i) {
      if (notNull == nullptr || notNull[i]) {
        boolStats->increase(1);
        boolStats->update(byteData[i], 1);
      } else if (!hasNull) {
        hasNull = true;
      }
    }
    boolStats->setHasNull(hasNull);
  }

  void BooleanColumnWriter::flush(std::vector<proto::Stream>& streams) {
    ColumnWriter::flush(streams);

    proto::Stream stream;
    stream.set_kind(proto::Stream_Kind_DATA);
    stream.set_column(static_cast<uint32_t>(columnId));
    stream.set_length(rleEncoder->flush());
    streams.push_back(stream);
  }

  uint64_t BooleanColumnWriter::getEstimatedSize() const {
    uint64_t size = ColumnWriter::getEstimatedSize();
    size += rleEncoder->getBufferSize();
    return size;
  }

  void BooleanColumnWriter::getColumnEncoding(
                       std::vector<proto::ColumnEncoding>& encodings) const {
    proto::ColumnEncoding encoding;
    encoding.set_kind(proto::ColumnEncoding_Kind_DIRECT);
    encoding.set_dictionarysize(0);
    encodings.push_back(encoding);
  }

  void BooleanColumnWriter::recordPosition() const {
    ColumnWriter::recordPosition();
    rleEncoder->recordPosition(rowIndexPosition.get());
  }

  class DoubleColumnWriter : public ColumnWriter {
  public:
    DoubleColumnWriter(const Type& type,
                       const StreamsFactory& factory,
                       const WriterOptions& options,
                       bool isFloat);

    virtual void add(ColumnVectorBatch& rowBatch,
                     uint64_t offset,
                     uint64_t numValues) override;

    virtual void flush(std::vector<proto::Stream>& streams) override;

    virtual uint64_t getEstimatedSize() const override;

    virtual void getColumnEncoding(
        std::vector<proto::ColumnEncoding>& encodings) const override;

    virtual void recordPosition() const override;

  private:
    bool isFloat;
    std::unique_ptr<AppendOnlyBufferedStream> dataStream;
    DataBuffer<char> buffer;
  };

  DoubleColumnWriter::DoubleColumnWriter(
                          const Type& type,
                          const StreamsFactory& factory,
                          const WriterOptions& options,
                          bool isFloatType) :
                              ColumnWriter(type, factory, options),
                              isFloat(isFloatType),
                              buffer(*options.getMemoryPool()) {
    dataStream.reset(new AppendOnlyBufferedStream(
                             factory.createStream(proto::Stream_Kind_DATA)));
    buffer.resize(isFloat ? 4 : 8);

    if (enableIndex) {
      recordPosition();
    }
  }

  void DoubleColumnWriter::add(ColumnVectorBatch& rowBatch,
                               uint64_t offset,
                               uint64_t numValues) {
    ColumnWriter::add(rowBatch, offset, numValues);

    const DoubleVectorBatch& dblBatch =
                             dynamic_cast<const DoubleVectorBatch&>(rowBatch);

    const double* doubleData = dblBatch.data.data() + offset;
    const char* notNull = dblBatch.hasNulls ?
                          dblBatch.notNull.data() + offset : nullptr;

    size_t bytes = isFloat ? 4 : 8;
    char* data = buffer.data();

    for (uint64_t i = 0; i < numValues; ++i) {
      if (!notNull || notNull[i]) {
        if (isFloat) {
          // to avoid float-double cast
          const int32_t* intBits =
            reinterpret_cast<const int32_t*>(&static_cast<const float&>(
              doubleData[i]));
          for (size_t j = 0; j < bytes; ++j) {
            data[j] = static_cast<char>(((*intBits) >> (8 * j)) & 0xff);
          }
        } else {
          const int64_t* intBits =
            reinterpret_cast<const int64_t*>(&(doubleData[i]));
          for (size_t j = 0; j < bytes; ++j) {
            data[j] = static_cast<char>(((*intBits) >> (8 * j)) & 0xff);
          }
        }
        dataStream->write(data, bytes);
      }
    }

    DoubleColumnStatisticsImpl* doubleStats =
        dynamic_cast<DoubleColumnStatisticsImpl*>(colIndexStatistics.get());
    bool hasNull = false;
    for (uint64_t i = 0; i < numValues; ++i) {
      if (!notNull || notNull[i]) {
        doubleStats->increase(1);
        doubleStats->update(doubleData[i]);
      } else if (!hasNull) {
        hasNull = true;
      }
    }
    doubleStats->setHasNull(hasNull);
  }

  void DoubleColumnWriter::flush(std::vector<proto::Stream>& streams) {
    ColumnWriter::flush(streams);

    proto::Stream stream;
    stream.set_kind(proto::Stream_Kind_DATA);
    stream.set_column(static_cast<uint32_t>(columnId));
    stream.set_length(dataStream->flush());
    streams.push_back(stream);
  }

  uint64_t DoubleColumnWriter::getEstimatedSize() const {
    uint64_t size = ColumnWriter::getEstimatedSize();
    size += dataStream->getSize();
    return size;
  }

  void DoubleColumnWriter::getColumnEncoding(
                      std::vector<proto::ColumnEncoding>& encodings) const {
    proto::ColumnEncoding encoding;
    encoding.set_kind(proto::ColumnEncoding_Kind_DIRECT);
    encoding.set_dictionarysize(0);
    encodings.push_back(encoding);
  }

  void DoubleColumnWriter::recordPosition() const {
    ColumnWriter::recordPosition();
    dataStream->recordPosition(rowIndexPosition.get());
  }

  class StringColumnWriter : public ColumnWriter {
  public:
    StringColumnWriter(const Type& type,
                       const StreamsFactory& factory,
                       const WriterOptions& options);

    virtual void add(ColumnVectorBatch& rowBatch,
                     uint64_t offset,
                     uint64_t numValues) override;

    virtual void flush(std::vector<proto::Stream>& streams) override;

    virtual uint64_t getEstimatedSize() const override;

    virtual void getColumnEncoding(
        std::vector<proto::ColumnEncoding>& encodings) const override;

    virtual void recordPosition() const override;

  protected:
    std::unique_ptr<RleEncoder> lengthEncoder;
    std::unique_ptr<AppendOnlyBufferedStream> dataStream;
    RleVersion rleVersion;
  };

  StringColumnWriter::StringColumnWriter(
                          const Type& type,
                          const StreamsFactory& factory,
                          const WriterOptions& options) :
                              ColumnWriter(type, factory, options),
                              rleVersion(RleVersion_1) {
    std::unique_ptr<BufferedOutputStream> lengthStream =
        factory.createStream(proto::Stream_Kind_LENGTH);
    lengthEncoder = createRleEncoder(std::move(lengthStream),
                                     false,
                                     rleVersion,
                                     memPool);
    dataStream.reset(new AppendOnlyBufferedStream(
        factory.createStream(proto::Stream_Kind_DATA)));

    if (enableIndex) {
      recordPosition();
    }
  }

  void StringColumnWriter::add(ColumnVectorBatch& rowBatch,
                               uint64_t offset,
                               uint64_t numValues) {
    ColumnWriter::add(rowBatch, offset, numValues);
    const StringVectorBatch & stringBatch =
      dynamic_cast<const StringVectorBatch &>(rowBatch);

    char *const * data = stringBatch.data.data() + offset;
    const int64_t* length = stringBatch.length.data() + offset;
    const char* notNull = stringBatch.hasNulls ?
                          stringBatch.notNull.data() + offset : nullptr;

    lengthEncoder->add(length, numValues, notNull);

    for (uint64_t i = 0; i < numValues; ++i) {
      if (!notNull || notNull[i]) {
        dataStream->write(data[i], static_cast<size_t>(length[i]));
      }
    }

    StringColumnStatisticsImpl* strStats =
        dynamic_cast<StringColumnStatisticsImpl*>(colIndexStatistics.get());

    bool hasNull = false;
    for (uint64_t i = 0; i < numValues; ++i) {
      if (!notNull || notNull[i]) {
        strStats->update(data[i],
                         static_cast<size_t>(length[i]));
        strStats->increase(1);
      } else if (!hasNull) {
        hasNull = true;
      }
    }
    strStats->setHasNull(hasNull);
  }

  void StringColumnWriter::flush(std::vector<proto::Stream>& streams) {
    ColumnWriter::flush(streams);

    proto::Stream length;
    length.set_kind(proto::Stream_Kind_LENGTH);
    length.set_column(static_cast<uint32_t>(columnId));
    length.set_length(lengthEncoder->flush());
    streams.push_back(length);

    proto::Stream data;
    data.set_kind(proto::Stream_Kind_DATA);
    data.set_column(static_cast<uint32_t>(columnId));
    data.set_length(dataStream->flush());
    streams.push_back(data);
  }

  uint64_t StringColumnWriter::getEstimatedSize() const {
    uint64_t size = ColumnWriter::getEstimatedSize();
    size += lengthEncoder->getBufferSize();
    size += dataStream->getSize();
    return size;
  }

  void StringColumnWriter::getColumnEncoding(
    std::vector<proto::ColumnEncoding>& encodings) const {
    proto::ColumnEncoding encoding;
    encoding.set_kind(rleVersion == RleVersion_1 ?
                      proto::ColumnEncoding_Kind_DIRECT :
                      proto::ColumnEncoding_Kind_DIRECT_V2);
    encoding.set_dictionarysize(0);
    encodings.push_back(encoding);
  }

  void StringColumnWriter::recordPosition() const {
    ColumnWriter::recordPosition();
    dataStream->recordPosition(rowIndexPosition.get());
    lengthEncoder->recordPosition(rowIndexPosition.get());
  }

  class CharColumnWriter : public StringColumnWriter {
  public:
    CharColumnWriter(const Type& type,
                     const StreamsFactory& factory,
                     const WriterOptions& options) :
                         StringColumnWriter(type, factory, options),
                         fixedLength(type.getMaximumLength()),
                         padBuffer(*options.getMemoryPool(),
                                   type.getMaximumLength()) {
      // PASS
    }

    virtual void add(ColumnVectorBatch& rowBatch,
                     uint64_t offset,
                     uint64_t numValues) override;

  private:
    uint64_t fixedLength;
    DataBuffer<char> padBuffer;
  };

  void CharColumnWriter::add(ColumnVectorBatch& rowBatch,
                             uint64_t offset,
                             uint64_t numValues) {
    ColumnWriter::add(rowBatch, offset, numValues);
    StringVectorBatch& charsBatch = dynamic_cast<StringVectorBatch&>(rowBatch);

    char** data = charsBatch.data.data() + offset;
    int64_t* length = charsBatch.length.data() + offset;
    const char* notNull = charsBatch.hasNulls ?
                          charsBatch.notNull.data() + offset : nullptr;

    StringColumnStatisticsImpl* strStats =
        dynamic_cast<StringColumnStatisticsImpl*>(colIndexStatistics.get());
    bool hasNull = false;

    for (uint64_t i = 0; i < numValues; ++i) {
      if (!notNull || notNull[i]) {
        char *charData = data[i];
        uint64_t oriLength = static_cast<uint64_t>(length[i]);
        if (oriLength < fixedLength) {
          memcpy(padBuffer.data(), data[i], oriLength);
          memset(padBuffer.data() + oriLength, ' ', fixedLength - oriLength);
          charData = padBuffer.data();
        }
        length[i] = static_cast<int64_t>(fixedLength);
        dataStream->write(charData, fixedLength);

        strStats->update(charData, fixedLength);
        strStats->increase(1);
      } else if (!hasNull) {
        hasNull = true;
      }
    }
    lengthEncoder->add(length, numValues, notNull);
    strStats->setHasNull(hasNull);
  }

  class VarCharColumnWriter : public StringColumnWriter {
  public:
    VarCharColumnWriter(const Type& type,
                        const StreamsFactory& factory,
                        const WriterOptions& options) :
                            StringColumnWriter(type, factory, options),
                            maxLength(type.getMaximumLength()) {
      // PASS
    }

    virtual void add(ColumnVectorBatch& rowBatch,
                     uint64_t offset,
                     uint64_t numValues) override;

  private:
    uint64_t maxLength;
  };

  void VarCharColumnWriter::add(ColumnVectorBatch& rowBatch,
                                uint64_t offset,
                                uint64_t numValues) {
    ColumnWriter::add(rowBatch, offset, numValues);
    StringVectorBatch& charsBatch = dynamic_cast<StringVectorBatch&>(rowBatch);

    char* const* data = charsBatch.data.data() + offset;
    int64_t* length = charsBatch.length.data() + offset;
    const char* notNull = charsBatch.hasNulls ?
                          charsBatch.notNull.data() + offset : nullptr;

    StringColumnStatisticsImpl* strStats =
        dynamic_cast<StringColumnStatisticsImpl*>(colIndexStatistics.get());
    bool hasNull = false;

    for (uint64_t i = 0; i < numValues; ++i) {
      if (!notNull || notNull[i]) {
        if (length[i] > static_cast<int64_t>(maxLength)) {
          length[i] = static_cast<int64_t>(maxLength);
        }
        dataStream->write(data[i], static_cast<size_t>(length[i]));

        strStats->update(data[i], static_cast<size_t>(length[i]));
        strStats->increase(1);
      } else if (!hasNull) {
        hasNull = true;
      }
    }
    lengthEncoder->add(length, numValues, notNull);
    strStats->setHasNull(hasNull);
  }

  class BinaryColumnWriter : public StringColumnWriter {
  public:
    BinaryColumnWriter(const Type& type,
                       const StreamsFactory& factory,
                       const WriterOptions& options) :
                           StringColumnWriter(type, factory, options) {
      // PASS
    }

    virtual void add(ColumnVectorBatch& rowBatch,
                     uint64_t offset,
                     uint64_t numValues) override;
  };

  void BinaryColumnWriter::add(ColumnVectorBatch& rowBatch,
                               uint64_t offset,
                               uint64_t numValues) {
    ColumnWriter::add(rowBatch, offset, numValues);

    StringVectorBatch & binBatch = dynamic_cast<StringVectorBatch &>(rowBatch);
    char** data = binBatch.data.data() + offset;
    int64_t* length = binBatch.length.data() + offset;
    const char* notNull = binBatch.hasNulls ?
                          binBatch.notNull.data() + offset : nullptr;

    BinaryColumnStatisticsImpl* binStats =
        dynamic_cast<BinaryColumnStatisticsImpl*>(colIndexStatistics.get());

    bool hasNull = false;
    for (uint64_t i = 0; i < numValues; ++i) {
      uint64_t unsignedLength = static_cast<uint64_t>(length[i]);
      if (!notNull || notNull[i]) {
        dataStream->write(data[i], unsignedLength);

        binStats->update(unsignedLength);
        binStats->increase(1);
      } else if (!hasNull) {
        hasNull = true;
      }
    }
    lengthEncoder->add(length, numValues, notNull);
    binStats->setHasNull(hasNull);
  }

  class TimestampColumnWriter : public ColumnWriter {
  public:
    TimestampColumnWriter(const Type& type,
                          const StreamsFactory& factory,
                          const WriterOptions& options);

    virtual void add(ColumnVectorBatch& rowBatch,
                     uint64_t offset,
                     uint64_t numValues) override;

    virtual void flush(std::vector<proto::Stream>& streams) override;

    virtual uint64_t getEstimatedSize() const override;

    virtual void getColumnEncoding(
        std::vector<proto::ColumnEncoding>& encodings) const override;

    virtual void recordPosition() const override;

  protected:
    std::unique_ptr<RleEncoder> secRleEncoder, nanoRleEncoder;

  private:
    RleVersion rleVersion;
    const Timezone& timezone;
  };

  TimestampColumnWriter::TimestampColumnWriter(
                             const Type& type,
                             const StreamsFactory& factory,
                             const WriterOptions& options) :
                                 ColumnWriter(type, factory, options),
                                 rleVersion(RleVersion_1),
                                 timezone(getLocalTimezone()){
    std::unique_ptr<BufferedOutputStream> dataStream =
        factory.createStream(proto::Stream_Kind_DATA);
    std::unique_ptr<BufferedOutputStream> secondaryStream =
        factory.createStream(proto::Stream_Kind_SECONDARY);
    secRleEncoder = createRleEncoder(std::move(dataStream),
                                     true,
                                     rleVersion,
                                     memPool);
    nanoRleEncoder = createRleEncoder(std::move(secondaryStream),
                                      false,
                                      rleVersion,
                                      memPool);

    if (enableIndex) {
      recordPosition();
    }
  }

  static int64_t formatNano(int64_t nanos) {
    if (nanos == 0) {
      return 0;
    } else if (nanos % 100 != 0) {
      return (nanos) << 3;
    } else {
      nanos /= 100;
      int64_t trailingZeros = 1;
      while (nanos % 10 == 0 && trailingZeros < 7) {
        nanos /= 10;
        trailingZeros += 1;
      }
      return (nanos) << 3 | trailingZeros;
    }
  }

  void TimestampColumnWriter::add(ColumnVectorBatch& rowBatch,
                                  uint64_t offset,
                                  uint64_t numValues) {
    ColumnWriter::add(rowBatch, offset, numValues);

    TimestampVectorBatch& tsBatch =
      dynamic_cast<TimestampVectorBatch &>(rowBatch);

    const char* notNull = tsBatch.hasNulls ?
                          tsBatch.notNull.data() + offset : nullptr;
    int64_t *secs = tsBatch.data.data() + offset;
    int64_t *nanos = tsBatch.nanoseconds.data() + offset;

    TimestampColumnStatisticsImpl* tsStats =
        dynamic_cast<TimestampColumnStatisticsImpl*>(colIndexStatistics.get());
    for (uint64_t i = 0; i < numValues; ++i) {
      if (notNull == nullptr || notNull[i]) {
        // TimestampVectorBatch already stores data in UTC
        int64_t millsUTC = secs[i] * 1000 + nanos[i] / 1000000;
        tsStats->increase(1);
        tsStats->update(millsUTC);
      } else if (!tsStats->hasNull()) {
        tsStats->setHasNull(true);
      }
    }

    for (uint64_t i = 0; i < numValues; ++i) {
      if (notNull == nullptr || notNull[i]) {
        secs[i] -= timezone.getVariant(secs[i]).gmtOffset;
        secs[i] -= timezone.getEpoch();
        nanos[i] = formatNano(nanos[i]);
      }
    }

    secRleEncoder->add(secs, numValues, notNull);
    nanoRleEncoder->add(nanos, numValues, notNull);
  }

  void TimestampColumnWriter::flush(std::vector<proto::Stream>& streams) {
    ColumnWriter::flush(streams);

    proto::Stream dataStream;
    dataStream.set_kind(proto::Stream_Kind_DATA);
    dataStream.set_column(static_cast<uint32_t>(columnId));
    dataStream.set_length(secRleEncoder->flush());
    streams.push_back(dataStream);

    proto::Stream secondaryStream;
    secondaryStream.set_kind(proto::Stream_Kind_SECONDARY);
    secondaryStream.set_column(static_cast<uint32_t>(columnId));
    secondaryStream.set_length(nanoRleEncoder->flush());
    streams.push_back(secondaryStream);
  }

  uint64_t TimestampColumnWriter::getEstimatedSize() const {
    uint64_t size = ColumnWriter::getEstimatedSize();
    size += secRleEncoder->getBufferSize();
    size += nanoRleEncoder->getBufferSize();
    return size;
  }

  void TimestampColumnWriter::getColumnEncoding(
    std::vector<proto::ColumnEncoding>& encodings) const {
    proto::ColumnEncoding encoding;
    encoding.set_kind(rleVersion == RleVersion_1 ?
                      proto::ColumnEncoding_Kind_DIRECT :
                      proto::ColumnEncoding_Kind_DIRECT_V2);
    encoding.set_dictionarysize(0);
    encodings.push_back(encoding);
  }

  void TimestampColumnWriter::recordPosition() const {
    ColumnWriter::recordPosition();
    secRleEncoder->recordPosition(rowIndexPosition.get());
    nanoRleEncoder->recordPosition(rowIndexPosition.get());
  }

  class DateColumnWriter : public IntegerColumnWriter {
  public:
    DateColumnWriter(const Type& type,
                     const StreamsFactory& factory,
                     const WriterOptions& options);

    virtual void add(ColumnVectorBatch& rowBatch,
                     uint64_t offset,
                     uint64_t numValues) override;
  };

  DateColumnWriter::DateColumnWriter(
                        const Type &type,
                        const StreamsFactory &factory,
                        const WriterOptions &options) :
                            IntegerColumnWriter(type, factory, options) {
    // PASS
  }

  void DateColumnWriter::add(ColumnVectorBatch& rowBatch,
                             uint64_t offset,
                             uint64_t numValues) {
    ColumnWriter::add(rowBatch, offset, numValues);

    const LongVectorBatch& longBatch =
      dynamic_cast<const LongVectorBatch&>(rowBatch);

    const int64_t* data = longBatch.data.data() + offset;
    const char* notNull = longBatch.hasNulls ?
                          longBatch.notNull.data() + offset : nullptr;

    rleEncoder->add(data, numValues, notNull);

    DateColumnStatisticsImpl* dateStats =
      dynamic_cast<DateColumnStatisticsImpl*>(colIndexStatistics.get());
    bool hasNull = false;
    for (uint64_t i = 0; i < numValues; ++i) {
      if (!notNull || notNull[i]) {
        dateStats->increase(1);
        dateStats->update(static_cast<int32_t>(data[i]));
      } else if (!hasNull) {
        hasNull = true;
      }
    }
    dateStats->setHasNull(hasNull);
  }

  class Decimal64ColumnWriter : public ColumnWriter {
  public:
    static const uint32_t MAX_PRECISION_64 = 18;
    static const uint32_t MAX_PRECISION_128 = 38;

    Decimal64ColumnWriter(const Type& type,
                          const StreamsFactory& factory,
                          const WriterOptions& options);

    virtual void add(ColumnVectorBatch& rowBatch,
                     uint64_t offset,
                     uint64_t numValues) override;

    virtual void flush(std::vector<proto::Stream>& streams) override;

    virtual uint64_t getEstimatedSize() const override;

    virtual void getColumnEncoding(
        std::vector<proto::ColumnEncoding>& encodings) const override;

    virtual void recordPosition() const override;

  protected:
    RleVersion rleVersion;
    uint64_t precision;
    uint64_t scale;
    std::unique_ptr<AppendOnlyBufferedStream> valueStream;
    std::unique_ptr<RleEncoder> scaleEncoder;

  private:
    char buffer[8];
  };

  Decimal64ColumnWriter::Decimal64ColumnWriter(
                             const Type& type,
                             const StreamsFactory& factory,
                             const WriterOptions& options) :
                                 ColumnWriter(type, factory, options),
                                 rleVersion(RleVersion_1),
                                 precision(type.getPrecision()),
                                 scale(type.getScale()) {
    valueStream.reset(new AppendOnlyBufferedStream(
        factory.createStream(proto::Stream_Kind_DATA)));
    std::unique_ptr<BufferedOutputStream> scaleStream =
        factory.createStream(proto::Stream_Kind_SECONDARY);
    scaleEncoder = createRleEncoder(std::move(scaleStream),
                                    true,
                                    rleVersion,
                                    memPool);

    if (enableIndex) {
      recordPosition();
    }
  }

  void Decimal64ColumnWriter::add(ColumnVectorBatch& rowBatch,
                                  uint64_t offset,
                                  uint64_t numValues) {
    ColumnWriter::add(rowBatch, offset, numValues);

    const Decimal64VectorBatch& decBatch =
      dynamic_cast<const Decimal64VectorBatch&>(rowBatch);

    const char* notNull = decBatch.hasNulls ?
                          decBatch.notNull.data() + offset : nullptr;
    const int64_t* values = decBatch.values.data() + offset;

    for (uint64_t i = 0; i < numValues; ++i) {
      if (!notNull || notNull[i]) {
        int64_t val = zigZag(values[i]);
        char* data = buffer;
        while (true) {
          if ((val & ~0x7f) == 0) {
            *(data++) = (static_cast<char>(val));
            break;
          } else {
            *(data++) = static_cast<char>(0x80 | (val & 0x7f));
            // cast val to unsigned so as to force 0-fill right shift
            val = (static_cast<uint64_t>(val) >> 7);
          }
        }
        valueStream->write(buffer, static_cast<size_t>(data - buffer));
      }
    }
    std::vector<int64_t> scales(numValues, static_cast<int64_t>(scale));
    scaleEncoder->add(scales.data(), numValues, notNull);

    DecimalColumnStatisticsImpl* decStats =
        dynamic_cast<DecimalColumnStatisticsImpl*>(colIndexStatistics.get());
    bool hasNull = false;
    for (uint64_t i = 0; i < numValues; ++i) {
      if (!notNull || notNull[i]) {
        decStats->increase(1);
        decStats->update(Decimal(values[i], static_cast<int32_t>(scale)));
      } else if (!hasNull) {
        hasNull = true;
      }
    }
    decStats->setHasNull(hasNull);
  }

  void Decimal64ColumnWriter::flush(std::vector<proto::Stream>& streams) {
    ColumnWriter::flush(streams);

    proto::Stream dataStream;
    dataStream.set_kind(proto::Stream_Kind_DATA);
    dataStream.set_column(static_cast<uint32_t>(columnId));
    dataStream.set_length(valueStream->flush());
    streams.push_back(dataStream);

    proto::Stream secondaryStream;
    secondaryStream.set_kind(proto::Stream_Kind_SECONDARY);
    secondaryStream.set_column(static_cast<uint32_t>(columnId));
    secondaryStream.set_length(scaleEncoder->flush());
    streams.push_back(secondaryStream);
  }

  uint64_t Decimal64ColumnWriter::getEstimatedSize() const {
    uint64_t size = ColumnWriter::getEstimatedSize();
    size += valueStream->getSize();
    size += scaleEncoder->getBufferSize();
    return size;
  }

  void Decimal64ColumnWriter::getColumnEncoding(
    std::vector<proto::ColumnEncoding>& encodings) const {
    proto::ColumnEncoding encoding;
    encoding.set_kind(proto::ColumnEncoding_Kind_DIRECT);
    encoding.set_dictionarysize(0);
    encodings.push_back(encoding);
  }

  void Decimal64ColumnWriter::recordPosition() const {
    ColumnWriter::recordPosition();
    valueStream->recordPosition(rowIndexPosition.get());
    scaleEncoder->recordPosition(rowIndexPosition.get());
  }

  class Decimal128ColumnWriter : public Decimal64ColumnWriter {
  public:
    Decimal128ColumnWriter(const Type& type,
                           const StreamsFactory& factory,
                           const WriterOptions& options);

    virtual void add(ColumnVectorBatch& rowBatch,
                     uint64_t offset,
                     uint64_t numValues) override;

  private:
    char buffer[16];
  };

  Decimal128ColumnWriter::Decimal128ColumnWriter(
                              const Type& type,
                              const StreamsFactory& factory,
                              const WriterOptions& options) :
                                Decimal64ColumnWriter(type, factory, options) {
    // PASS
  }

  Int128 zigZagInt128(const Int128& value) {
    bool isNegative = value < 0;
    Int128 val = value.abs();
    val <<= 1;
    if (isNegative) {
      val -= 1;
    }
    return val;
  }

  void Decimal128ColumnWriter::add(ColumnVectorBatch& rowBatch,
                                   uint64_t offset,
                                   uint64_t numValues) {
    ColumnWriter::add(rowBatch, offset, numValues);

    const Decimal128VectorBatch & decBatch =
      dynamic_cast<const Decimal128VectorBatch &>(rowBatch);

    const char* notNull = decBatch.hasNulls ?
                          decBatch.notNull.data() + offset : nullptr;
    const Int128* values = decBatch.values.data() + offset;

    for (uint64_t i = 0; i < numValues; ++i) {
      if (!notNull || notNull[i]) {
        Int128 val = zigZagInt128(values[i]);
        char* data = buffer;
        while (true) {
          if ((val & ~0x7f) == 0) {
            *(data++) = (static_cast<char>(val.getLowBits()));
            break;
          } else {
            *(data++) = static_cast<char>(0x80 | (val.getLowBits() & 0x7f));
            val >>= 7;
          }
        }
        valueStream->write(buffer, static_cast<size_t>(data - buffer));
      }
    }
    std::vector<int64_t> scales(numValues, static_cast<int64_t>(scale));
    scaleEncoder->add(scales.data(), numValues, notNull);

    DecimalColumnStatisticsImpl* decStats =
        dynamic_cast<DecimalColumnStatisticsImpl*>(colIndexStatistics.get());
    bool hasNull = false;
    for (uint64_t i = 0; i < numValues; ++i) {
      if (!notNull || notNull[i]) {
        decStats->increase(1);
        decStats->update(Decimal(values[i], static_cast<int32_t>(scale)));
      } else if (!hasNull) {
        hasNull = true;
      }
    }
    decStats->setHasNull(hasNull);
  }

  std::unique_ptr<ColumnWriter> buildWriter(
                                            const Type& type,
                                            const StreamsFactory& factory,
                                            const WriterOptions& options) {
    switch (static_cast<int64_t>(type.getKind())) {
      case STRUCT:
        return std::unique_ptr<ColumnWriter>(
          new StructColumnWriter(
                                 type,
                                 factory,
                                 options));
      case INT:
      case LONG:
      case SHORT:
        return std::unique_ptr<ColumnWriter>(
          new IntegerColumnWriter(
                                  type,
                                  factory,
                                  options));
      case BYTE:
        return std::unique_ptr<ColumnWriter>(
          new ByteColumnWriter(
                               type,
                               factory,
                               options));
      case BOOLEAN:
        return std::unique_ptr<ColumnWriter>(
          new BooleanColumnWriter(
                                  type,
                                  factory,
                                  options));
      case DOUBLE:
        return std::unique_ptr<ColumnWriter>(
          new DoubleColumnWriter(
                                 type,
                                 factory,
                                 options,
                                 false));
      case FLOAT:
        return std::unique_ptr<ColumnWriter>(
          new DoubleColumnWriter(
                                 type,
                                 factory,
                                 options,
                                 true));
      case BINARY:
        return std::unique_ptr<ColumnWriter>(
          new BinaryColumnWriter(
                                 type,
                                 factory,
                                 options));
      case STRING:
        return std::unique_ptr<ColumnWriter>(
          new StringColumnWriter(
                                 type,
                                 factory,
                                 options));
      case CHAR:
        return std::unique_ptr<ColumnWriter>(
          new CharColumnWriter(
                               type,
                               factory,
                               options));
      case VARCHAR:
        return std::unique_ptr<ColumnWriter>(
          new VarCharColumnWriter(
                                  type,
                                  factory,
                                  options));
      case DATE:
        return std::unique_ptr<ColumnWriter>(
          new DateColumnWriter(
                               type,
                               factory,
                               options));
      case TIMESTAMP:
        return std::unique_ptr<ColumnWriter>(
          new TimestampColumnWriter(
                                    type,
                                    factory,
                                    options));
      case DECIMAL:
        if (type.getPrecision() <= Decimal64ColumnWriter::MAX_PRECISION_64) {
          return std::unique_ptr<ColumnWriter>(
            new Decimal64ColumnWriter(
                                      type,
                                      factory,
                                      options));
        } else if (type.getPrecision() <= Decimal64ColumnWriter::MAX_PRECISION_128) {
          return std::unique_ptr<ColumnWriter>(
            new Decimal128ColumnWriter(
                                       type,
                                       factory,
                                       options));
        } else {
          throw NotImplementedYet("Decimal precision more than 38 is not "
                                    "supported");
        }
      default:
        throw NotImplementedYet("Type is not supported yet for creating "
                                  "ColumnWriter.");
    }
  }
}
