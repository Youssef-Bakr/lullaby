/*
Copyright 2017 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS-IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef LULLABY_UTIL_FLATBUFFER_WRITER_H_
#define LULLABY_UTIL_FLATBUFFER_WRITER_H_

#include <memory>
#include "flatbuffers/flatbuffers.h"
#include "lullaby/util/flatbuffer_native_types.h"
#include "lullaby/util/inward_buffer.h"
#include "lullaby/util/logging.h"
#include "lullaby/util/optional.h"

namespace lull {

// Writes a flatbuffer from an instance of an object class generated by the
// Lullaby flatc code generator.
//
// The FlatBufferWriter uses an InwardBuffer to write the data as a flatbuffer.
// Flatbuffers are built in "bottom-up" order.  The InwardBuffer allows
// temporary data to be written to "low" memory while the actual flatbuffer is
// written to "high" memory backwards.  This minimizes the amount of memory
// needed for creating a flatbuffer.
//
// The main data structure stored in flatbuffers are flatbuffers::Table objects.
// A Table is divided into two parts: the vtable and the main table.  The vtable
// contains information about what data is stored in the main table.  The main
// table contains either scalar types (eg. ints, floats, etc.), structs (a group
// of scalar types), or references to other objects in the flatbuffer.
// References are represented as offsets relative to the location of the
// reference data itself.  The vtable stores information about whether or not a
// given data field is present in the main table and where it is located (as an
// offset into the main table data).
//
// In addition to flatbuffer::Tables, a flatbuffer can store String and Vector
// container types.  A flatbuffer::String is a pointer to a block of data
// containing the size of the string followed by the string contents (with a
// null terminator).  A flatbuffer::Vector is a pointer to a block of data
// containing the size of the vector followed by the vector contents.
//
// While writing a table, information about the vtable and some data is stored
// in the "low" memory of the InwardBuffer while actual objects (Tables,
// Vectors, and Strings) are created in the "high" memory.  When a table is
// "finished", the main table and vtable are written to the "high" memory by
// processing the information in the "low" memory.  By delaying the writing of
// the table until it is "finished", nested tables can be supported.  The nested
// table is "finished" into "high" memory and a reference field is added to the
// parent's "low" memory.  Strings and Vectors are handled similarly by adding
// the data to "high" memory and adding a reference field to "low" memory.
class FlatbufferWriter {
 public:
  template <typename T>
  static void* SerializeObject(T* obj, InwardBuffer* buffer) {
    const size_t start = buffer->FrontSize();

    // Write the obj to the buffer as a flatbuffer table.
    FlatbufferWriter writer(buffer);

    const size_t table_start = writer.StartTable();
    obj->SerializeFlatbuffer(writer);
    const size_t table_end = writer.EndTable(table_start);
    writer.Finish(table_end);

    const size_t end = buffer->FrontSize();
    if (start != end) {
      LOG(DFATAL) << "Unexpected temporary memory left behind.";
    }

    // Return a pointer to the finished flatbuffer.
    return buffer->BackAt(buffer->BackSize());
  }

  explicit FlatbufferWriter(InwardBuffer* buffer) : buffer_(buffer) {}

  // Serializes a scalar value (eg. uint8, int32, float, double, etc.).
  template <typename T, typename U>
  void Scalar(T* value, uint16_t offset, U default_value) {
    AddValueField(offset / 2, *value);
  }

  // Serializes a reference to a table or string already in the buffer.  The
  // |value| must be a pointer to data already serialized into the buffer.
  template <typename T>
  void Reference(T* value, uint16_t offset) {
    const void* end = buffer_->BackAt(0);
    const size_t reference =
        reinterpret_cast<uintptr_t>(end) - reinterpret_cast<uintptr_t>(value);
    Reference(reference, offset);
  }

  // Serializes a reference to a table or string already in the buffer.  The
  // |reference| must be within the current buffer bounds.
  void Reference(size_t reference, uint16_t offset) {
    CHECK_LE(reference, buffer_->BackSize());
    AddReferenceField(offset / 2, reference);
  }

  // Serializes a string.
  void String(std::string* value, uint16_t offset) {
    const size_t reference = CreateString(*value);
    AddReferenceField(offset / 2, reference);
  }

  // Serializes a flatbuffer struct-type.
  template <typename T>
  void Struct(T* value, uint16_t offset) {
    AddValueField(offset / 2, *value);
  }

  // Serializes an optional flatbuffer struct-type.  If the value is set, it
  // simply calls Struct().
  template <typename T>
  void Struct(lull::Optional<T>* value, uint16_t offset) {
    if (value->get()) {
      Struct(value->get(), offset);
    }
  }

  // Serializes a flatbuffer struct-type that has specified a "native_type"
  // attribute.  In this case, the code generator does not generate any native
  // code and, instead, uses the specified native_type.
  template <typename T>
  void NativeStruct(T* value, uint16_t offset) {
    const size_t size = FlatbufferNativeType<T>::kFlatbufferStructSize;
    const size_t align = FlatbufferNativeType<T>::kFlatbufferStructAlignment;
    void* ptr = AddValueField(offset / 2, size, align);
    FlatbufferNativeType<T>::Write(*value, ptr, size);
  }

  // Serializes an optional flatbuffer struct-type that has specified a
  // "native_type" attribute.  If the value is set, it simply calls
  // NativeStruct().
  template <typename T>
  void NativeStruct(lull::Optional<T>* value, uint16_t offset) {
    if (value->get()) {
      NativeStruct(value->get(), offset);
    }
  }

  // Serializes a flatbuffer table.
  template <typename T>
  void Table(T* value, uint16_t offset) {
    const size_t start = StartTable();
    value->SerializeFlatbuffer(*this);
    const size_t reference = EndTable(start);
    AddReferenceField(offset / 2, reference);
  }

  // Serializes an optional flatbuffer table.  If the value is set, it simply
  // calls Table().
  template <typename T>
  void Table(lull::Optional<T>* value, uint16_t offset) {
    if (value->get()) {
      Table(value->get(), offset);
    }
  }

  // Serializes a dynamic flatbuffer table.  If the value is set, it simply
  // calls Table().
  template <typename T>
  void Table(std::shared_ptr<T>* value, uint16_t offset) {
    if (value->get()) {
      Table(value->get(), offset);
    }
  }

  // Serializes a flatbuffer union type.
  template <typename T, typename U>
  void Union(T* value, uint16_t offset, U default_type_value) {
    const auto type = value->type();
    const uint16_t type_offset = static_cast<uint16_t>(offset - 2);
    AddValueField(type_offset / 2, type);

    if (type == 0) {
      AddReferenceField(offset / 2, 0);
    } else {
      const size_t start = StartTable();
      value->SerializeFlatbuffer(type, *this);
      const size_t reference = EndTable(start);
      AddReferenceField(offset / 2, reference);
    }
  }

  // Serializes an array of scalar values.
  template <typename T, typename U = T>
  void VectorOfScalars(std::vector<T>* value, uint16_t offset) {
    const size_t start = StartVector();
    for (auto iter = value->rbegin(); iter != value->rend(); ++iter) {
      const U u = *iter;
      AddVectorValue(&u);
    }
    const size_t reference = EndVector(start, value->size());
    AddReferenceField(offset / 2, reference);
  }

  // Serializes an array of strings.
  void VectorOfStrings(std::vector<std::string>* value, uint16_t offset) {
    const size_t start = StartVector();
    for (const std::string& str : *value) {
      AddVectorReference(CreateString(str));
    }
    const size_t reference = EndVector(start, value->size());
    AddReferenceField(offset / 2, reference);
  }

  // Serializes an array of flatbuffer struct types.
  template <typename T>
  void VectorOfStructs(std::vector<T>* value, uint16_t offset) {
    const size_t start = StartVector();
    for (auto iter = value->rbegin(); iter != value->rend(); ++iter) {
      const T& t = *iter;
      AddVectorValue(&t);
    }
    const size_t reference = EndVector(start, value->size());
    AddReferenceField(offset / 2, reference);
  }

  // Serializes an array of flatbuffer struct types that have specified a
  // "native_type" attribute.
  template <typename T>
  void VectorOfNativeStructs(std::vector<T>* value, uint16_t offset) {
    const size_t start = StartVector();
    for (auto iter = value->rbegin(); iter != value->rend(); ++iter) {
      const size_t size = FlatbufferNativeType<T>::kFlatbufferStructSize;
      void* ptr = buffer_->AllocBack(size);
      FlatbufferNativeType<T>::Write(*iter, ptr, size);
    }
    const size_t reference = EndVector(start, value->size());
    AddReferenceField(offset / 2, reference);
  }

  // Serializes an array of flatbuffer table types.
  template <typename T>
  void VectorOfTables(std::vector<T>* value, uint16_t offset) {
    const size_t start = StartVector();
    for (T& table : *value) {
      const size_t table_start = StartTable();
      table.SerializeFlatbuffer(*this);
      const size_t table_end = EndTable(table_start);
      AddVectorReference(table_end);
    }
    const size_t reference = EndVector(start, value->size());
    AddReferenceField(offset / 2, reference);
  }

  // Informs objects that this serializer will not overwrite data.
  bool IsDestructive() const { return false; }

  // Readies this writer for creating a flatbuffer table.  The value returned by
  // this function must be passed into EndTable (see below).  Between the
  // StartTable/EndTable functions, users can call the Scalar, Struct, Table,
  // etc. functions to build the table data.  Reference types (eg. string,
  // table, union, vector) must be written before value types (eg. scalar,
  // struct, native struct).
  size_t StartTable() {
    return buffer_->FrontSize();
  }

  // Finishes writing a table to the flatbuffer.  Specifically, this function
  // will write all data into the "object" section of the flatbuffer (fixing up
  // any references), then write the data for the "vtable".  This function must
  // be called after StartTable and the value returned by StartTable must be
  // passed in as |start|.  This function returns the offset position of the
  // table root in the InwardBuffer.
  size_t EndTable(size_t start) {
    const size_t end = buffer_->FrontSize();

    size_t object_size = 0;
    size_t vtable_size = 0;
    const size_t root_offset =
        WriteTable(start, end, &object_size, &vtable_size);

    const size_t vtable_offset = CreateVTable(vtable_size, object_size);
    UpdateVTable(start, end, root_offset, vtable_offset);

    buffer_->EraseFront(end - start);
    return root_offset;
  }

  // Readies this writer for creating a flatbuffer vector.  The value returned
  // by this function must be passed into EndVector (see below).  Between the
  // StartVector/EndVector functions, users can add elements to the vector
  // by calling AddVectorValue or AddVectorReference.
  size_t StartVector() {
    return buffer_->FrontSize();
  }

  // Adds a value to a vector that has been readied using StartVector.  Values
  // are added in reverse order.
  template <typename T>
  void AddVectorValue(T* value) {
    buffer_->WriteBack(*value);
  }

  // Adds a reference to a vector that has been readied using StartVector.
  void AddVectorReference(size_t reference) {
    buffer_->WriteFront(static_cast<uint32_t>(reference));
  }

  // Finishes writing a vector to the flatbuffer.  Specifically, this function
  // will fix up any references that were added to the vector using
  // AddVectorReference, then write the length of the vector into the
  // flatbuffer. This function must be called after StartVector and the value
  // returned by StartVector must be passed in as |start|.  Additionally, the
  // number of elements that were written into the vector must be passed as the
  // |num| argument.  This function returns the offset position of the vector in
  // the InwardBuffer.
  size_t EndVector(size_t start, size_t num) {
    const size_t end = buffer_->FrontSize();
    if (start != end) {
      for (size_t i = 0; i < num; ++i) {
        const size_t size = buffer_->FrontSize();
        const uint32_t* ptr =
            reinterpret_cast<uint32_t*>(buffer_->FrontAt(size));
        const uint32_t offset = *(ptr - 1);
        WriteReference(offset);
        buffer_->EraseFront(sizeof(uint32_t));
      }
    }

    if (num == 0) {
      return 0;
    } else {
      buffer_->WriteBack(static_cast<uint32_t>(num));
      return buffer_->BackSize();
    }
  }

  // Finishes a table as a root of the flatbuffer.  This allows users to call
  // flatbuffers::GetRoot<T> on the data stored in the buffer.
  const void* Finish(size_t root) {
    WriteReference(root);
    return buffer_->BackAt(buffer_->BackSize());
  }

 private:
  void Prealign(size_t alignment) {
    while ((buffer_->BackSize()) % alignment != 0) {
      buffer_->WriteBack<uint8_t>(0);
    }
  }

  template <typename T>
  void AddValueField(uint16_t index, const T& value) {
    Prealign(alignof(T));
    buffer_->WriteBack(value);

    Field field;
    field.index = index;
    field.size = sizeof(T);
    field.align = alignof(T);
    field.offset = static_cast<uint32_t>(buffer_->BackSize());
    buffer_->WriteFront(field);
  }

  void* AddValueField(uint16_t index, size_t size, size_t align) {
    Prealign(align);
    buffer_->AllocBack(size);

    Field field;
    field.index = index;
    field.size = static_cast<uint8_t>(size);
    field.align = static_cast<uint8_t>(align);
    field.offset = static_cast<uint32_t>(buffer_->BackSize());
    buffer_->WriteFront(field);

    return buffer_->BackAt(buffer_->BackSize());
  }

  void AddReferenceField(uint16_t index, size_t reference) {
    Field field;
    field.index = index;
    field.offset = static_cast<uint32_t>(reference);
    buffer_->WriteFront(field);
  }

  void WriteReference(size_t reference) {
    const size_t end = buffer_->BackSize() + sizeof(uint32_t);
    const uint32_t offset = static_cast<uint32_t>(end - reference);
    buffer_->WriteBack(offset);
  }

  size_t CreateString(const std::string& str) {
    if (str.empty()) {
      return 0;
    }
    buffer_->WriteBack(static_cast<uint8_t>(0));  // Null terminator.
    buffer_->WriteBack(str.data(), str.length());
    buffer_->WriteBack(static_cast<uint32_t>(str.length()));
    return buffer_->BackSize();
  }

  size_t WriteTable(size_t start, size_t end, size_t* object_size,
                    size_t* vtable_size) {
    size_t max_field = 2;
    for (size_t iter = start; iter < end; iter += sizeof(Field)) {
      Field* field = reinterpret_cast<Field*>(buffer_->FrontAt(iter));

      max_field = std::max<size_t>(max_field, field->index);
      if (field->size == 0 && field->offset == 0) {
        // Null reference, do nothing.
        continue;
      }

      if (field->size == 0) {
        WriteReference(field->offset);
        // Reacquire the field pointer in case the buffer was reallocated after
        // writing the reference.
        field = reinterpret_cast<Field*>(buffer_->FrontAt(iter));
        field->offset = static_cast<uint32_t>(buffer_->BackSize());
        *object_size += sizeof(field->offset);
      } else {
        *object_size += field->size;
      }
    }

    // The vtable is entirely made up of uint16_t entries.  The first two
    // entries are the size of the vtable (in bytes) and the size of the
    // object data.
    *vtable_size = (max_field + 1) * sizeof(uint16_t);
    // Offset to vtable from the root of the table as int32_t.  A positive
    // value of N indicates the vtable is N bytes lower than the root.  In
    // our case, the vtable is directly ahead of the table root.
    const int32_t offset_to_vtable = static_cast<int32_t>(*vtable_size);
    buffer_->WriteBack<int32_t>(offset_to_vtable);

    return buffer_->BackSize();
  }

  size_t CreateVTable(size_t vtable_size, size_t object_size) {
    // Reserve the block of memory to actually fill the vtable offset data.
    // Initialize the block to zeros which indicates no such field.
    const size_t offsets_size = vtable_size - (2 * sizeof(uint16_t));
    memset(buffer_->AllocBack(offsets_size), 0, offsets_size);
    const size_t vtable_offset = buffer_->BackSize();

    // The first two entries in the vtable are the vtable size and the object
    // table size.
    buffer_->WriteBack(static_cast<uint16_t>(object_size));
    buffer_->WriteBack(static_cast<uint16_t>(vtable_size));
    return vtable_offset;
  }

  void UpdateVTable(size_t start, size_t end, size_t root_offset,
                    size_t vtable_offset) {
    uint16_t* offsets =
        reinterpret_cast<uint16_t*>(buffer_->BackAt(vtable_offset));

    // Update the vtable with information about the actual data.
    for (size_t iter = start; iter < end; iter += sizeof(Field)) {
      const Field* field = reinterpret_cast<Field*>(buffer_->FrontAt(iter));
      if (field->offset == 0) {
        continue;
      }

      const uint16_t offset =
          static_cast<uint16_t>(root_offset - field->offset);
      offsets[field->index - 2] = offset;
    }
  }

  struct Field {
    uint16_t index = 0;
    uint8_t size = 0;
    uint8_t align = 0;
    uint32_t offset = 0;
  };

  InwardBuffer* buffer_ = nullptr;
};

template <typename T>
inline void* WriteFlatbuffer(T* obj, InwardBuffer* buffer) {
  return FlatbufferWriter::SerializeObject(obj, buffer);
}

}  // namespace lull

#endif  // LULLABY_UTIL_FLATBUFFER_WRITER_H_
