/*
 * Copyright (c) 1997, 2019, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2014, 2018, Red Hat Inc. All rights reserved.
 * Copyright (c) 2020, 2021, Huawei Technologies Co., Ltd. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "code/compiledIC.hpp"
#include "code/icBuffer.hpp"
#include "code/nmethod.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/safepoint.hpp"

// ----------------------------------------------------------------------------

#define __ _masm.
address CompiledStaticCall::emit_to_interp_stub(CodeBuffer &cbuf, address mark) {
  precond(cbuf.stubs()->start() != badAddress);
  precond(cbuf.stubs()->end() != badAddress);
  // Stub is fixed up when the corresponding call is converted from
  // calling compiled code to calling interpreted code.
  // mv xmethod, 0
  // jalr -4 # to self

  if (mark == NULL) {
    mark = cbuf.insts_mark();  // Get mark within main instrs section.
  }

  // Note that the code buffer's insts_mark is always relative to insts.
  // That's why we must use the macroassembler to generate a stub.
  MacroAssembler _masm(&cbuf);

  address base = __ start_a_stub(to_interp_stub_size());
  int offset = __ offset();
  if (base == NULL) {
    return NULL;  // CodeBuffer::expand failed
  }
  // static stub relocation stores the instruction address of the call
  __ relocate(static_stub_Relocation::spec(mark));

  __ emit_static_call_stub();

  assert((__ offset() - offset) <= (int)to_interp_stub_size(), "stub too big");
  __ end_a_stub();
  return base;
}
#undef __

int CompiledStaticCall::to_interp_stub_size() {
  // (lui, addi, slli, addi, slli, addi) + (lui, addi, slli, addi, slli) + jalr
  return 12 * NativeInstruction::instruction_size;
}

int CompiledStaticCall::to_trampoline_stub_size() {
  // Somewhat pessimistically, we count 4 instructions here (although
  // there are only 3) because we sometimes emit an alignment nop.
  // Trampoline stubs are always word aligned.
  return NativeInstruction::instruction_size + NativeCallTrampolineStub::instruction_size;
}

// Relocation entries for call stub, compiled java to interpreter.
int CompiledStaticCall::reloc_to_interp_stub() {
  return 4; // 3 in emit_to_interp_stub + 1 in emit_call
}

void CompiledDirectStaticCall::set_to_interpreted(const methodHandle& callee, address entry) {
  address stub = find_stub(false /* is_aot */);
  guarantee(stub != NULL, "stub not found");

  if (TraceICs) {
    ResourceMark rm;
    tty->print_cr("CompiledDirectStaticCall@" INTPTR_FORMAT ": set_to_interpreted %s",
                  p2i(instruction_address()),
                  callee->name_and_sig_as_C_string());
  }

  // Creation also verifies the object.
  NativeMovConstReg* method_holder = nativeMovConstReg_at(stub);
#ifndef PRODUCT
  NativeGeneralJump* jump = nativeGeneralJump_at(method_holder->next_instruction_address());

  // read the value once
  volatile intptr_t data = method_holder->data();
  assert(data == 0 || data == (intptr_t)callee(),
         "a) MT-unsafe modification of inline cache");
  assert(data == 0 || jump->jump_destination() == entry,
         "b) MT-unsafe modification of inline cache");
#endif
  // Update stub.
  method_holder->set_data((intptr_t)callee());
  NativeGeneralJump::insert_unconditional(method_holder->next_instruction_address(), entry);
  ICache::invalidate_range(stub, to_interp_stub_size());
  // Update jump to call.
  set_destination_mt_safe(stub);
}

void CompiledDirectStaticCall::set_stub_to_clean(static_stub_Relocation* static_stub) {
  assert (CompiledIC_lock->is_locked() || SafepointSynchronize::is_at_safepoint(), "mt unsafe call");
  // Reset stub.
  address stub = static_stub->addr();
  assert(stub != NULL, "stub not found");
  // Creation also verifies the object.
  NativeMovConstReg* method_holder = nativeMovConstReg_at(stub);
  method_holder->set_data(0);
}

//-----------------------------------------------------------------------------
// Non-product mode code
#ifndef PRODUCT

void CompiledDirectStaticCall::verify() {
  // Verify call.
  _call->verify();
  _call->verify_alignment();

  // Verify stub.
  address stub = find_stub(false /* is_aot */);
  assert(stub != NULL, "no stub found for static call");
  // Creation also verifies the object.
  NativeMovConstReg* method_holder = nativeMovConstReg_at(stub);
  NativeJump* jump = nativeJump_at(method_holder->next_instruction_address());

  // Verify state.
  assert(is_clean() || is_call_to_compiled() || is_call_to_interpreted(), "sanity check");
}

#endif // !PRODUCT
