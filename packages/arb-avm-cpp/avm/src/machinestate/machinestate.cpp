/*
 * Copyright 2019-2020, Offchain Labs, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <avm/machinestate/machinestate.hpp>

#include <avm/machinestate/machineoperation.hpp>
#include <avm_values/exceptions.hpp>
#include <avm_values/vmValueParser.hpp>

#include <ethash/keccak.hpp>

#include <iostream>

namespace {
uint256_t max_arb_gas_remaining = std::numeric_limits<uint256_t>::max();
}  // namespace

AssertionContext::AssertionContext(
    std::vector<Tuple> inbox_messages,
    Tuple sideload,
    bool blockingSideload_,
    nonstd::optional<value> fake_inbox_peek_value_)
    : inbox_messages(std::move(inbox_messages)),
      inbox_messages_consumed(0),
      sideload_value(std::move(sideload)),
      numSteps{0},
      numGas{0},
      blockingSideload(blockingSideload_),
      fake_inbox_peek_value(std::move(fake_inbox_peek_value_)) {}

MachineState::MachineState()
    : arb_gas_remaining(max_arb_gas_remaining),
      pc(0, 0),
      errpc({0, 0}, getErrCodePoint()),
      staged_message(Tuple()) {}

MachineState::MachineState(std::shared_ptr<Code> code_, value static_val_)
    : code(std::move(code_)),
      static_val(std::move(static_val_)),
      arb_gas_remaining(max_arb_gas_remaining),
      pc(code->initialCodePointRef()),
      errpc({0, 0}, code->loadCodePoint({0, 0})),
      staged_message(Tuple()) {}

MachineState::MachineState(std::shared_ptr<Code> code_,
                           value register_val_,
                           value static_val_,
                           Datastack stack_,
                           Datastack auxstack_,
                           uint256_t arb_gas_remaining_,
                           Status state_,
                           CodePointRef pc_,
                           CodePointStub errpc_,
                           Tuple staged_message_)
    : code(std::move(code_)),
      registerVal(std::move(register_val_)),
      static_val(static_val_),
      stack(std::move(stack_)),
      auxstack(std::move(auxstack_)),
      arb_gas_remaining(arb_gas_remaining_),
      state(state_),
      pc(pc_),
      errpc(errpc_),
      staged_message(std::move(staged_message_)) {}

MachineState MachineState::loadFromFile(
    const std::string& executable_filename) {
    auto executable = loadExecutable(executable_filename);
    auto code = std::make_shared<Code>(0);
    code->addSegment(std::move(executable.code));
    return MachineState{std::move(code), std::move(executable.static_val)};
}

uint256_t MachineState::hash() const {
    if (state == Status::Halted)
        return 0;
    if (state == Status::Error)
        return 1;

    std::array<unsigned char, 32 * 8> data;
    auto oit = data.begin();
    {
        auto val = ::hash(loadCurrentInstruction());
        oit = to_big_endian(val, oit);
    }
    {
        auto val = stack.hash();
        oit = to_big_endian(val, oit);
    }
    {
        auto val = auxstack.hash();
        oit = to_big_endian(val, oit);
    }
    {
        auto val = ::hash_value(registerVal);
        oit = to_big_endian(val, oit);
    }
    {
        auto val = ::hash_value(static_val);
        oit = to_big_endian(val, oit);
    }
    { oit = to_big_endian(arb_gas_remaining, oit); }
    {
        auto val = ::hash_value(errpc);
        oit = to_big_endian(val, oit);
    }
    {
        auto val = ::hash_value(staged_message);
        to_big_endian(val, oit);
    }

    auto hash_val = ethash::keccak256(data.data(), data.size());
    return intx::be::load<uint256_t>(hash_val);
}

uint256_t MachineState::getMachineSize() {
    uint256_t machine_size = 0;

    machine_size += getSize(static_val);
    machine_size += getSize(registerVal);
    machine_size += stack.getTotalValueSize();
    machine_size += auxstack.getTotalValueSize();

    return machine_size;
}

namespace {
void marshalState(std::vector<unsigned char>& buf,
                  const Code& code,
                  uint256_t next_codepoint_hash,
                  HashPreImage stackPreImage,
                  HashPreImage auxStackPreImage,
                  value registerVal,
                  value staticVal,
                  uint256_t arb_gas_remaining,
                  CodePointStub errpc,
                  const Tuple& staged_message) {
    marshal_uint256_t(next_codepoint_hash, buf);

    stackPreImage.marshal(buf);
    auxStackPreImage.marshal(buf);

    ::marshalForProof(registerVal, MarshalLevel::STUB, buf, code);
    ::marshalForProof(staticVal, MarshalLevel::STUB, buf, code);
    marshal_uint256_t(arb_gas_remaining, buf);
    marshal_uint256_t(::hash(errpc), buf);
    ::marshalForProof(staged_message, MarshalLevel::SINGLE, buf, code);
}
}  // namespace

std::vector<unsigned char> MachineState::marshalState() const {
    auto stackPreImage = stack.getHashPreImage();
    auto auxStackPreImage = auxstack.getHashPreImage();
    std::vector<unsigned char> buf;

    ::marshalState(buf, *code, ::hash(loadCurrentInstruction()), stackPreImage,
                   auxStackPreImage, registerVal, static_val, arb_gas_remaining,
                   errpc, staged_message);
    return buf;
}

std::vector<unsigned char> bufferToVec(const Buffer& b) {
    std::vector<unsigned char> res;
    uint64_t size = b.size();
    std::cerr << "Buffer size " << size << std::endl;
    while (size > 0 && b.get(size-1) == 0) {
        // std::cerr << "Buffer size " << size << std::endl;
        size--;
    }
    uint64_t size_ext = b.size();
    if (size_ext < 32) size_ext = 32;
    while (size_ext/2 >= size && size_ext > 32) {
        size_ext = size_ext/2;
    }
    std::cerr << "Buffer size ext " << size_ext << std::endl;
    for (uint64_t i = 0; i < size_ext; i++) {
        res.push_back(b.get(i));
    }
    std::cerr << "Buffer size " << size << " ext " << size_ext << " orig " << b.size() << std::endl;
    return res;
}

uint256_t merkleHash(uint8_t *buf, int offset, int sz) {
    if (sz == 32) {
        auto hash_val = ethash::keccak256(buf+offset, 32);
        uint256_t res = intx::be::load<uint256_t>(hash_val);
        return res;
    }
    // std::cerr << "hashing " << offset << " to " << (offset+sz) << std::endl;
    auto h1 = merkleHash(buf, offset, sz/2);
    auto h2 = merkleHash(buf, offset+sz/2, sz/2);
    return hash(h1, h2);
}

std::vector<unsigned char> makeProof(uint8_t *arr, uint64_t offset, uint64_t sz, uint64_t loc) {
    if (sz == 32) {
        auto res = std::vector<unsigned char>(arr+loc, arr+loc+32);
        /*
        for (int i = 0; i < 32; i++) {
            std::cerr << "hmm " << i << " " << int(arr[loc+i]) << " " << int(res[i]) << std::endl;
        }
        */
        return res;
        // return std::vector<unsigned char>();
    } else if (loc < offset + sz/2) {
        auto proof = makeProof(arr, offset, sz/2, loc);
        marshal_uint256_t(merkleHash(arr, offset+sz/2, sz/2), proof);
        return proof;
    } else {
        auto proof = makeProof(arr, offset+sz/2, sz/2, loc);
        marshal_uint256_t(merkleHash(arr, offset, sz/2), proof);
        return proof;
    }
}

std::vector<unsigned char> makeProof(Buffer &buf, uint64_t loc) {
    auto arr = bufferToVec(buf);
    auto res = makeProof(arr.data(), 0, arr.size(), ((loc/32) % (arr.size()/32))*32);
    std::cerr << "Making " << arr.size() << " -- " << res.size()/32 << std::endl;
    return res;
}

std::vector<unsigned char> makeNormalizationProof(uint8_t *arr, uint64_t sz) {
    std::vector<unsigned char> res;
    for (int i = 0; i < 31; i++) {
        res.push_back(0);
    }

    if (sz == 32) {
        std::cerr << "Simple normalization" << std::endl;
        res.push_back(0);
        marshal_uint256_t(merkleHash(arr, 0, sz), res);
        marshal_uint256_t(merkleHash(arr, 0, sz), res);
        return res;
    }

    res.push_back(makeProof(arr, 0, sz, 0).size()/32);
    marshal_uint256_t(merkleHash(arr, 0, sz/2), res);
    marshal_uint256_t(merkleHash(arr, sz/2, sz/2), res);
    return res;
}

std::vector<unsigned char> makeNormalizationProof(Buffer &buf) {
    auto arr = bufferToVec(buf);
    std::cerr << "Making normal " << arr.size() << std::endl;
    return makeNormalizationProof(arr.data(), arr.size());
}

void insertSizes(std::vector<unsigned char> &buf, int sz1, int sz2, int sz3, int sz4) {
    int acc = 1;
    buf.push_back(static_cast<uint8_t>(acc));
    // std::cerr << "Setting sizes " << acc << std::endl;
    acc += sz1/32;
    buf.push_back(static_cast<uint8_t>(acc));
    // std::cerr << "Setting sizes " << acc << std::endl;
    acc += sz2/32;
    buf.push_back(static_cast<uint8_t>(acc));
    // std::cerr << "Setting sizes " << acc << std::endl;
    acc += sz3/32;
    buf.push_back(static_cast<uint8_t>(acc));
    // std::cerr << "Setting sizes " << acc << std::endl;
    acc += sz4/32;
    buf.push_back(static_cast<uint8_t>(acc));
    // std::cerr << "Setting sizes " << acc << std::endl;
    for (int i = 5; i < 32; i++) {
        buf.push_back(0);
    }
}

void makeSetBufferProof(std::vector<unsigned char> &buf, uint64_t loc, Buffer buffer, uint256_t v, int wordSize) {
    Buffer nbuffer = buffer;
    Buffer nbuffer1 = nbuffer;
    bool aligned = true;
    for (int i = 0; i < wordSize; i++) {
        if ((loc + i) % 32 == 0 && i > 0) {
            std::cerr << "Unaligned " << std::endl;
            nbuffer1 = nbuffer;
            aligned = false;
        }
        std::cerr << "Setting to " << (loc+i) << " " << int(static_cast<uint8_t>((v >> ((wordSize-1-i)*8)) & 0xff)) << " size " << nbuffer.size() << std::endl;
        nbuffer = nbuffer.set(loc + i, static_cast<uint8_t>((v >> ((wordSize-1-i)*8)) & 0xff));
    }
    auto proof1 = makeProof(buffer, loc);
    auto nproof1 = makeNormalizationProof(nbuffer1);

    if (aligned) {
        insertSizes(buf, proof1.size(), nproof1.size(), 0, 0);
        buf.insert(buf.end(), proof1.begin(), proof1.end());
        buf.insert(buf.end(), nproof1.begin(), nproof1.end());
    } else {
        auto proof2 = makeProof(nbuffer1, loc + (wordSize-1));
        std::cerr << "Loc 1" << std::endl;
        auto nproof2 = makeNormalizationProof(nbuffer);
        std::cerr << "Loc 2" << std::endl;
        insertSizes(buf, proof1.size(), nproof1.size(), proof2.size(), nproof2.size());
        buf.insert(buf.end(), proof1.begin(), proof1.end());
        buf.insert(buf.end(), nproof1.begin(), nproof1.end());
        buf.insert(buf.end(), proof2.begin(), proof2.end());
        buf.insert(buf.end(), nproof2.begin(), nproof2.end());
    }
}

std::vector<unsigned char> MachineState::marshalBufferProof() {
    std::vector<unsigned char> buf;
    auto opcode = loadCurrentInstruction().op.opcode;
    if (opcode < OpCode::GET_BUFFER8 || opcode > OpCode::SET_BUFFER256) {
        return buf;
    } 
    // Find the buffer
    auto buffer = nonstd::get_if<Buffer>(&stack[0]);
    if (!buffer) {
        return buf;
    }
    // Also need the offset
    auto offset = nonstd::get_if<uint256_t>(&stack[1]);
    if (!offset) {
        return buf;
    }
    if (*offset > std::numeric_limits<uint64_t>::max()) {
        return buf;
    }
    auto loc = static_cast<uint64_t>(*offset);
    if (opcode == OpCode::GET_BUFFER8) {
        auto proof = makeProof(*buffer, loc);
        insertSizes(buf, proof.size(), 0, 0, 0);
        buf.insert(buf.end(), proof.begin(), proof.end());
    } else if (opcode == OpCode::GET_BUFFER64) {
        auto proof1 = makeProof(*buffer, loc);
        auto proof2 = makeProof(*buffer, loc+7);
        insertSizes(buf, proof1.size(), 0, proof2.size(), 0);
        buf.insert(buf.end(), proof1.begin(), proof1.end());
        buf.insert(buf.end(), proof2.begin(), proof2.end());
    } else if (opcode == OpCode::GET_BUFFER256) {
        auto proof1 = makeProof(*buffer, loc);
        auto proof2 = makeProof(*buffer, loc+31);
        insertSizes(buf, proof1.size(), 0, proof2.size(), 0);
        buf.insert(buf.end(), proof1.begin(), proof1.end());
        buf.insert(buf.end(), proof2.begin(), proof2.end());
    } else {
        auto val = nonstd::get_if<uint256_t>(&stack[2]);
        if (!val) {
          return buf;
        }
        if (opcode == OpCode::SET_BUFFER8) {
            // std::cerr << "Here " << intx::to_string(buffer->hash(), 16) << std::endl;
            Buffer nbuffer = buffer->set(loc, static_cast<uint8_t>(*val));
            // std::cerr << "Making proof" << std::endl;
            auto proof1 = makeProof(*buffer, loc);
            // std::cerr << "Proof: " << int(proof1[0]) << " -- " << int(proof1[31]) << std::endl;
            // std::cerr << "Normalize" << std::endl;
            auto nproof1 = makeNormalizationProof(nbuffer);
            insertSizes(buf, proof1.size(), nproof1.size(), 0, 0);
            buf.insert(buf.end(), proof1.begin(), proof1.end());
            buf.insert(buf.end(), nproof1.begin(), nproof1.end());
        } else if (opcode == OpCode::SET_BUFFER64) {
            makeSetBufferProof(buf, loc, *buffer, *val, 8);
        } else if (opcode == OpCode::SET_BUFFER256) {
            makeSetBufferProof(buf, loc, *buffer, *val, 32);
        }
    }

    return buf;
}

std::vector<unsigned char> MachineState::marshalForProof() {
    auto currentInstruction = loadCurrentInstruction();
    auto& current_op = currentInstruction.op;
    auto opcode = current_op.opcode;
    std::vector<MarshalLevel> stackPops = InstructionStackPops.at(opcode);
    std::vector<MarshalLevel> auxStackPops = InstructionAuxStackPops.at(opcode);

    uint64_t stack_pop_count = stackPops.size();

    MarshalLevel immediateMarshalLevel = MarshalLevel::STUB;
    if (current_op.immediate) {
        if (stackPops.empty()) {
            stack_pop_count++;
        } else {
            immediateMarshalLevel = stackPops[0];
            stackPops.erase(stackPops.begin());
        }
    }

    std::vector<unsigned char> buf;
    buf.push_back(stack_pop_count);
    buf.push_back(auxStackPops.size());

    auto stackProof = stack.marshalForProof(stackPops, *code);
    auto auxStackProof = auxstack.marshalForProof(auxStackPops, *code);

    buf.insert(buf.end(), stackProof.second.begin(), stackProof.second.end());
    if (current_op.immediate) {
        ::marshalForProof(*current_op.immediate, immediateMarshalLevel, buf,
                          *code);
    }
    buf.insert(buf.end(), auxStackProof.second.begin(),
               auxStackProof.second.end());
    ::marshalState(buf, *code, currentInstruction.nextHash, stackProof.first,
                   auxStackProof.first, registerVal, static_val,
                   arb_gas_remaining, errpc, staged_message);

    buf.push_back(current_op.immediate ? 1 : 0);
    buf.push_back(static_cast<uint8_t>(current_op.opcode));
    return buf;
}

BlockReason MachineState::isBlocked(bool newMessages) const {
    if (state == Status::Error) {
        return ErrorBlocked();
    } else if (state == Status::Halted) {
        return HaltBlocked();
    }
    auto& instruction = loadCurrentInstruction();
    if (instruction.op.opcode == OpCode::INBOX ||
        instruction.op.opcode == OpCode::INBOX_PEEK) {
        if (newMessages) {
            return NotBlocked();
        }
        return InboxBlocked();
    } else {
        return NotBlocked();
    }
}

const CodePoint& MachineState::loadCurrentInstruction() const {
    if (!loaded_segment || loaded_segment->segment->segmentID() != pc.segment) {
        loaded_segment = code->loadCodeSegment(pc.segment);
    }
    return (*loaded_segment->segment)[pc.pc];
}

uint64_t MachineState::nextGasCost() const {
    auto& instruction = loadCurrentInstruction();
    auto base_gas =
        instructionGasCosts()[static_cast<size_t>(instruction.op.opcode)];
    if (instruction.op.opcode == OpCode::ECPAIRING) {
        base_gas += machineoperation::ec_pairing_variable_gas_cost(*this);
    }
    return base_gas;
}

BlockReason MachineState::runOne() {
    if (state == Status::Error) {
        return ErrorBlocked();
    }

    if (state == Status::Halted) {
        return HaltBlocked();
    }

    auto& instruction = loadCurrentInstruction();

    // We're only blocked if we can't execute at all
    BlockReason blockReason = [&]() -> BlockReason {
        // Always push the immediate to the stack if we're not blocked
        if (instruction.op.immediate) {
            auto imm = *instruction.op.immediate;
            stack.push(std::move(imm));
        }

        if (!instructionValidity()[static_cast<size_t>(
                instruction.op.opcode)]) {
            // The opcode is invalid, execute by transitioning to the error
            // state
            state = Status::Error;
            return NotBlocked();
        }

        uint64_t gas_cost = nextGasCost();
        if (arb_gas_remaining < gas_cost) {
            // If there's insufficient gas remaining, execute by transitioning
            // to the error state with remaining gas set to max
            arb_gas_remaining = max_arb_gas_remaining;
            state = Status::Error;
            return NotBlocked();
        }
        arb_gas_remaining -= gas_cost;

        // save stack size for stack cleanup in case of error
        uint64_t startStackSize = stack.stacksize();
        BlockReason blockReason = NotBlocked();
        try {
            blockReason = runOp(instruction.op.opcode);
        } catch (const std::exception&) {
            state = Status::Error;
        }

        if (!nonstd::holds_alternative<NotBlocked>(blockReason)) {
            // Get rid of the immediate and reset the gas if the machine was
            // actually blocked
            arb_gas_remaining += gas_cost;
            if (instruction.op.immediate) {
                stack.popClear();
            }
            return blockReason;
        }

        // adjust for the gas used
        context.numGas += gas_cost;

        if (state == Status::Error) {
            // if state is Error, clean up stack
            // Clear stack to base for instruction
            auto stackItems =
                InstructionStackPops.at(instruction.op.opcode).size();
            while (stack.stacksize() > 0 &&
                   startStackSize - stack.stacksize() < stackItems) {
                stack.popClear();
            }
        }

        return NotBlocked();
    }();

    if (nonstd::holds_alternative<NotBlocked>(blockReason)) {
        context.numSteps++;
    }

    // If we're in the error state, jump to the error handler if one is set
    if (state == Status::Error && !errpc.is_error()) {
        pc = errpc.pc;
        state = Status::Extensive;
    }

    return blockReason;
}

BlockReason MachineState::runOp(OpCode opcode) {
    // std::cerr << "Running opcode <" << InstructionNames.at(opcode) << ">" << std::hex << static_cast<int>(opcode) << std::endl;

    switch (opcode) {
            /**************************/
            /*  Arithmetic Operations */
            /**************************/
        case OpCode::ADD:
            machineoperation::add(*this);
            break;
        case OpCode::MUL:
            machineoperation::mul(*this);
            break;
        case OpCode::SUB:
            machineoperation::sub(*this);
            break;
        case OpCode::DIV:
            machineoperation::div(*this);
            break;
        case OpCode::SDIV:
            machineoperation::sdiv(*this);
            break;
        case OpCode::MOD:
            machineoperation::mod(*this);
            break;
        case OpCode::SMOD:
            machineoperation::smod(*this);
            break;
        case OpCode::ADDMOD:
            machineoperation::addmod(*this);
            break;
        case OpCode::MULMOD:
            machineoperation::mulmod(*this);
            break;
        case OpCode::EXP:
            machineoperation::exp(*this);
            break;
        case OpCode::SIGNEXTEND:
            machineoperation::signExtend(*this);
            break;
            /******************************************/
            /*  Comparison & Bitwise Logic Operations */
            /******************************************/
        case OpCode::LT:
            machineoperation::lt(*this);
            break;
        case OpCode::GT:
            machineoperation::gt(*this);
            break;
        case OpCode::SLT:
            machineoperation::slt(*this);
            break;
        case OpCode::SGT:
            machineoperation::sgt(*this);
            break;
        case OpCode::EQ:
            machineoperation::eq(*this);
            break;
        case OpCode::ISZERO:
            machineoperation::iszero(*this);
            break;
        case OpCode::BITWISE_AND:
            machineoperation::bitwiseAnd(*this);
            break;
        case OpCode::BITWISE_OR:
            machineoperation::bitwiseOr(*this);
            break;
        case OpCode::BITWISE_XOR:
            machineoperation::bitwiseXor(*this);
            break;
        case OpCode::BITWISE_NOT:
            machineoperation::bitwiseNot(*this);
            break;
        case OpCode::BYTE:
            machineoperation::byte(*this);
            break;
        case OpCode::SHL:
            machineoperation::shl(*this);
            break;
        case OpCode::SHR:
            machineoperation::shr(*this);
            break;
        case OpCode::SAR:
            machineoperation::sar(*this);
            break;

            /***********************/
            /*  Hashing Operations */
            /***********************/
        case OpCode::HASH:
            machineoperation::hashOp(*this);
            break;
        case OpCode::TYPE:
            machineoperation::typeOp(*this);
            break;
        case OpCode::ETHHASH2:
            machineoperation::ethhash2Op(*this);
            break;
        case OpCode::KECCAKF:
            machineoperation::keccakF(*this);
            break;
        case OpCode::SHA256F:
            machineoperation::sha256F(*this);
            break;

            /***********************************************/
            /*  Stack, Memory, Storage and Flow Operations */
            /***********************************************/
        case OpCode::POP:
            machineoperation::pop(*this);
            break;
        case OpCode::SPUSH:
            machineoperation::spush(*this);
            break;
        case OpCode::RPUSH:
            machineoperation::rpush(*this);
            break;
        case OpCode::RSET:
            machineoperation::rset(*this);
            break;
        case OpCode::JUMP:
            machineoperation::jump(*this);
            break;
        case OpCode::CJUMP:
            machineoperation::cjump(*this);
            break;
        case OpCode::STACKEMPTY:
            machineoperation::stackEmpty(*this);
            break;
        case OpCode::PCPUSH:
            machineoperation::pcPush(*this);
            break;
        case OpCode::AUXPUSH:
            machineoperation::auxPush(*this);
            break;
        case OpCode::AUXPOP:
            machineoperation::auxPop(*this);
            break;
        case OpCode::AUXSTACKEMPTY:
            machineoperation::auxStackEmpty(*this);
            break;
        case OpCode::NOP:
            ++pc;
            break;
        case OpCode::ERRPUSH:
            machineoperation::errPush(*this);
            break;
        case OpCode::ERRSET:
            machineoperation::errSet(*this);
            break;
            /****************************************/
            /*  Duplication and Exchange Operations */
            /****************************************/
        case OpCode::DUP0:
            machineoperation::dup0(*this);
            break;
        case OpCode::DUP1:
            machineoperation::dup1(*this);
            break;
        case OpCode::DUP2:
            machineoperation::dup2(*this);
            break;
        case OpCode::SWAP1:
            machineoperation::swap1(*this);
            break;
        case OpCode::SWAP2:
            machineoperation::swap2(*this);
            break;
            /*********************/
            /*  Tuple Operations */
            /*********************/
        case OpCode::TGET:
            machineoperation::tget(*this);
            break;
        case OpCode::TSET:
            machineoperation::tset(*this);
            break;
        case OpCode::TLEN:
            machineoperation::tlen(*this);
            break;
        case OpCode::XGET:
            machineoperation::xget(*this);
            break;
        case OpCode::XSET:
            machineoperation::xset(*this);
            break;
            /***********************/
            /*  Logging Operations */
            /***********************/
        case OpCode::BREAKPOINT:
            return machineoperation::breakpoint(*this);
        case OpCode::LOG:
            machineoperation::log(*this);
            break;
        case OpCode::DEBUG_PRINT:
            machineoperation::debug(*this);
            break;
            /**********************/
            /*  System Operations */
            /**********************/
        case OpCode::SEND: {
            auto send_results = machineoperation::send(*this);

            if (send_results == false) {
                std::cerr << "Send failure: over size limit" << std::endl;
            }

            break;
        }
        case OpCode::INBOX_PEEK:
            return machineoperation::inboxPeekOp(*this);
        case OpCode::INBOX:
            return machineoperation::inboxOp(*this);
        case OpCode::ERROR:
            state = Status::Error;
            break;
        case OpCode::HALT:
            state = Status::Halted;
            break;
        case OpCode::SET_GAS:
            machineoperation::setgas(*this);
            break;
        case OpCode::PUSH_GAS:
            machineoperation::pushgas(*this);
            break;
        case OpCode::ERR_CODE_POINT:
            machineoperation::errcodept(*this);
            break;
        case OpCode::PUSH_INSN:
            machineoperation::pushinsn(*this);
            break;
        case OpCode::PUSH_INSN_IMM:
            machineoperation::pushinsnimm(*this);
            break;
        case OpCode::SIDELOAD:
            machineoperation::sideload(*this);
            break;
        case OpCode::NEW_BUFFER:
            machineoperation::newbuffer(*this);
            break;
        case OpCode::GET_BUFFER8:
            machineoperation::getbuffer8(*this);
            break;
        case OpCode::GET_BUFFER64:
            machineoperation::getbuffer64(*this);
            break;
        case OpCode::GET_BUFFER256:
            machineoperation::getbuffer256(*this);
            break;
        case OpCode::SET_BUFFER8:
            machineoperation::setbuffer8(*this);
            break;
        case OpCode::SET_BUFFER64:
            machineoperation::setbuffer64(*this);
            break;
        case OpCode::SET_BUFFER256:
            machineoperation::setbuffer256(*this);
            break;
        /*****************/
        /*  Precompiles  */
        /*****************/
        case OpCode::ECRECOVER:
            machineoperation::ec_recover(*this);
            break;
        case OpCode::ECADD:
            machineoperation::ec_add(*this);
            break;
        case OpCode::ECMUL:
            machineoperation::ec_mul(*this);
            break;
        case OpCode::ECPAIRING:
            machineoperation::ec_pairing(*this);
            break;
        default:
            std::cerr << "Unhandled opcode <" << InstructionNames.at(opcode)
                      << ">" << std::hex << static_cast<int>(opcode);
            state = Status::Error;
    }

    return NotBlocked{};
}

std::ostream& operator<<(std::ostream& os, const MachineState& val) {
    os << "hash " << intx::to_string(val.hash(), 16) << "\n";
    os << "status " << static_cast<int>(val.state) << "\n";
    os << "pc " << val.pc << "\n";
    os << "data stack: " << val.stack << "\n";
    auto& current_code_point = val.code->loadCodePoint(val.pc);
    os << "operation " << current_code_point.op << "\n";
    os << "codePointHash " << intx::to_string(hash(current_code_point), 16)
       << "\n";
    os << "stackHash " << intx::to_string(val.stack.hash(), 16) << "\n";
    os << "auxStackHash " << intx::to_string(val.auxstack.hash(), 16) << "\n";
    os << "registerHash " << intx::to_string(hash_value(val.registerVal), 16)
       << "\n";
    os << "staticHash " << intx::to_string(hash_value(val.static_val), 16)
       << "\n";
    os << "arb_gas_remaining " << val.arb_gas_remaining << "\n";
    os << "err handler " << val.errpc.pc << "\n";
    auto& err_code_point = val.code->loadCodePoint(val.errpc.pc);
    os << "errHandlerHash " << intx::to_string(hash(err_code_point), 16)
       << "\n";
    return os;
}
