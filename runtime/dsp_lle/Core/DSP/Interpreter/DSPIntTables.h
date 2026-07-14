// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Core/DSP/DSPCommon.h"

namespace DSP::Interpreter
{
class Interpreter;

using InterpreterFunction = void (Interpreter::*)(UDSPInstruction);

struct DecodedInterpreterOp
{
  InterpreterFunction main;
  InterpreterFunction extension;
};

InterpreterFunction GetOp(UDSPInstruction inst);
InterpreterFunction GetExtOp(UDSPInstruction inst);
const DecodedInterpreterOp& GetDecodedOp(UDSPInstruction inst);
void InitInstructionTables();
void FinalizeInstructionTables();
}  // namespace DSP::Interpreter
