// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_INTERPRETER_HANDLER_TABLE_BUILDER_H_
#define V8_INTERPRETER_HANDLER_TABLE_BUILDER_H_

#include "src/handles.h"
#include "src/interpreter/bytecode-register.h"
#include "src/interpreter/bytecodes.h"
#include "src/zone-containers.h"

namespace v8 {
namespace internal {

class HandlerTable;
class Isolate;

namespace interpreter {

// A helper class for constructing exception handler tables for the interpreter.
class HandlerTableBuilder final BASE_EMBEDDED {
 public:
  explicit HandlerTableBuilder(Zone* zone);

  // Builds the actual handler table by copying the current values into a heap
  // object. Any further mutations to the builder won't be reflected.
  Handle<HandlerTable> ToHandlerTable(Isolate* isolate);

  // Creates a new handler table entry and returns a {hander_id} identifying the
  // entry, so that it can be referenced by below setter functions.
  int NewHandlerEntry();

  // Setter functions that modify certain values within the handler table entry
  // being referenced by the given {handler_id}. All values will be encoded by
  // the resulting {HandlerTable} class when copied into the heap.
  void SetTryRegionStart(int handler_id, size_t offset);
  void SetTryRegionEnd(int handler_id, size_t offset);
  void SetHandlerTarget(int handler_id, size_t offset);
  void SetPrediction(int handler_id, HandlerTable::CatchPrediction prediction);
  void SetContextRegister(int handler_id, Register reg);

 private:
  struct Entry {
    size_t offset_start;   // Bytecode offset starting try-region.
    size_t offset_end;     // Bytecode offset ending try-region.
    size_t offset_target;  // Bytecode offset of handler target.
    Register context;      // Register holding context for handler.
                           // Optimistic prediction for handler.
    HandlerTable::CatchPrediction catch_prediction_;
  };

  ZoneVector<Entry> entries_;

  DISALLOW_COPY_AND_ASSIGN(HandlerTableBuilder);
};

}  // namespace interpreter
}  // namespace internal
}  // namespace v8

#endif  // V8_INTERPRETER_HANDLER_TABLE_BUILDER_H_