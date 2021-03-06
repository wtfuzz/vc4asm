/*
 * Parser.cpp
 *
 *  Created on: 30.08.2014
 *      Author: mueller
 */

#include "Parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <cctype>

#include "Parser.tables.cpp"


string Parser::location::toString() const
{	return stringf("%s (%u)", File.c_str(), Line);
}


Parser::saveContext::saveContext(Parser& parent, fileContext* ctx)
:	Parent(parent)
{	parent.Context.emplace_back(ctx);
}

Parser::saveContext::~saveContext()
{	Parent.Context.pop_back();
}

Parser::saveLineContext::saveLineContext(Parser& parent, fileContext* ctx)
:	saveContext(parent, ctx)
, LineBak(parent.Line)
, AtBak(parent.At)
{}

Parser::saveLineContext::~saveLineContext()
{	strncpy(Parent.Line, LineBak.c_str(), sizeof(Line));
	Parent.At = AtBak;
}


string Parser::enrichMsg(string msg)
{	// Show context
	contextType type = CTX_CURRENT;
	for (auto i = Context.rbegin(); i != Context.rend(); ++i)
	{	const fileContext& ctx = **i;
		if (ctx.Line)
			switch (type)
			{case CTX_CURRENT:
				msg = stringf("%s (%u,%u): ", ctx.File.c_str(), ctx.Line, At - Line - Token.size() + 1) + msg;
				break;
			 case CTX_INCLUDE:
				msg += stringf("\n  Included from %s (%u)", ctx.File.c_str(), ctx.Line);
				break;
			 case CTX_MACRO:
				msg += stringf("\n  At invocation of macro from %s (%u)", ctx.File.c_str(), ctx.Line);
				break;
			 case CTX_FUNCTION:
				msg += stringf("\n  At function invocation from %s (%u)", ctx.File.c_str(), ctx.Line);
				break;
			 case CTX_ROOT:;
			}
		type = ctx.Type;
	}
	return msg;
}

void Parser::Fail(const char* fmt, ...)
{	Success = false;
	va_list va;
	va_start(va, fmt);
	throw enrichMsg(vstringf(fmt, va));
	va_end(va);
}

static const char msgpfx[][10]
{	"ERROR: "
,	"Warning: "
,	"Info: "
};

void Parser::Msg(severity level, const char* fmt, ...)
{	if (!Pass2 || Verbose < level)
		return;
	va_list va;
	va_start(va, fmt);
	fputs(msgpfx[level], stderr);
	fputs(enrichMsg(vstringf(fmt, va)).c_str(), stderr);
	va_end(va);
	fputc('\n', stderr);
}

void Parser::FlagsSize(size_t min)
{	if (InstFlags.size() < min)
		InstFlags.resize(min, IF_NONE);
}

void Parser::StoreInstruction(uint64_t inst)
{
	for (unsigned i = Back; i--;)
		Instructions[PC+i+1] = Instructions[PC+i];
	Instructions[PC++] = inst;
}

Parser::token_t Parser::NextToken()
{
	At += strspn(At, " \t\r\n");

	switch (*At)
	{case 0:
	 case '#':
		Token.clear();
		return END;
	 case '*':
	 case '>':
	 case '<':
	 case '&':
	 case '^':
	 case '|':
		if (At[1] == At[0])
		{	if (At[2] == At[1])
			{	Token.assign(At, 3);
				At += 3;
				return OP;
			}
		 op2:
			Token.assign(At, 2);
			At += 2;
			return OP;
		}
	 case '!':
	 case '=':
		if (At[1] == '=')
			goto op2;
	 case '+':
	 case '-':
	 case '/':
	 case '%':
	 case '~':
		Token.assign(At, 1);
		++At;
		return OP;
	 case '(':
	 case ')':
	 case '[':
	 case ']':
	 case '.':
	 case ',':
	 case ';':
	 case ':':
		Token.assign(At, 1);
		return (token_t)*At++;
	}

	size_t i = isdigit(*At)
		?	strspn(At, "0123456789abcdefABCDEFx.") // read until we don't find a hex digit, x (for hex) or .
		:	strcspn(At, ".,;:+-*/%()&|^~!=<> \t\r\n");
	Token.assign(At, i);
	At += i;

	return isdigit(Token[0]) ? NUM : WORD;
}

size_t Parser::parseUInt(const char* src, uint32_t& dst)
{	dst = 0;
	const char* cp = src;
	uint32_t basis = 10;
	uint32_t limit = UINT32_MAX / 10;
	for (char c; (c = *cp) != 0; ++cp)
	{	uint32_t digit = c - '0';
		if (digit >= 10)
		{	digit = toupper(c) - 'A';
			if (digit < 6)
				digit += 10;
			else
			{	if (dst == 0 && cp - src == 1)
					switch (c)
					{	case 'b': basis = 2; limit = UINT32_MAX/2; continue;
						case 'o': basis = 8; limit = UINT32_MAX/8; continue;
						case 'x': basis = 16; limit = UINT32_MAX/16; continue;
						case 'd': continue;
					}
				break;
			}
		}
		if (dst > limit)
			break;
		dst *= basis;
		if (dst > UINT32_MAX - digit)
			break;
		dst += digit;
	}
	return cp - src;
}

exprValue Parser::parseElemInt()
{	uint32_t value = 0;
	int pos = 0;
	signed char sign = 0;
	switch (Instruct.Sig)
	{case Inst::S_NONE:
		if (Instruct.Unpack == Inst::U_32
			&& Instruct.OpA == Inst::A_NOP && Instruct.OpM == Inst::M_NOP
			&& Instruct.RAddrA == Inst::R_NOP && Instruct.RAddrB == Inst::R_NOP )
			break;
	 default:
	 fail:
		Fail("Cannot combine load per QPU element with any other instruction.");
	 case Inst::S_LDI:
		if (Instruct.LdMode & Inst::L_SEMA)
			goto fail;
		if (Instruct.LdMode)
			sign = Instruct.LdMode == Inst::L_PEU ? 1 : -1;
	}
	while (true)
	{	auto val = ParseExpression();
		if (val.Type != V_INT)
			Fail("Only integers are allowed between [...].");
		if (val.iValue < -2 || val.iValue > 3)
			Fail("Load per QPU element can only deal with integer constants in the range of [-2..3].");
		if (val.iValue < 0)
		{	if (sign > 0)
				Fail("All integers in load per QPU element must be either in the range [-2..1] or in the range [0..3].");
			sign = -1;
		} else if (val.iValue > 1)
		{	if (sign < 0)
				Fail("All integers in load per QPU element must be either in the range [-2..1] or in the range [0..3].");
			sign = 1;
		}
		value |= (val.uValue * 0x8001 & 0x10001) << pos;

		switch (NextToken())
		{case END:
			Fail("Incomplete expression.");
		 default:
			Fail("Unexpected '%s' in per QPU element constant.", Token.c_str());
		 case COMMA:
			if (++pos >= 16)
				Fail("Too many initializers for per QPU element constant.");
			continue;
		 case SQBRC2:;
		}
		break;
	}
	// Make LDIPEx
	return exprValue(value, (valueType)(sign + V_LDPE));
}

exprValue Parser::ParseExpression()
{
	Eval eval;
	exprValue value;
	try
	{next:
		switch (NextToken())
		{case WORD:
			{	// Expand constants
				for (auto i = Context.end(); i != Context.begin(); )
				{	auto c = (*--i)->Consts.find(Token);
					if (c != (*i)->Consts.end())
					{	value = c->second.Value;
						goto have_value;
					}
				}
			}
			{	// try function
				auto fp = Functions.find(Token);
				if (fp != Functions.end())
				{	// Hit!
					value = doFUNC(fp);
					break;
				}
			}
			{	// try functional macro
				auto mp = MacroFuncs.find(Token);
				if (mp != MacroFuncs.end())
				{	value = doFUNCMACRO(mp);
					break;
				}
			}
			{	// try register
				const regEntry* rp = binary_search(regMap, Token.c_str());
				if (rp)
				{	value = rp->Value;
					break;
				}
			}
			// Try label prefix
			{	string identifier = Token;
				if (NextToken() != COLON)
					Fail("Invalid expression. The identifier %s did not evaluate.", identifier.c_str());
				if (identifier != "r")
					Fail("'%s:' is no valid label prefix.", identifier.c_str());
			}
		 case COLON: // Label
			{	// Is forward reference?
				bool forward = false;
				switch (NextToken())
				{default:
					Fail("Expected Label after ':'.");
				 case SQBRC1:
					// Internal register constant
					{	reg_t reg;
						value = ParseExpression();
						if (value.Type != V_INT)
							Fail("Expecting integer constant, found %s.", type2string(value.Type));
						reg.Num = (uint8_t)value.uValue;
						if (NextToken() != COMMA)
							Fail("Expected ',', found '%s'.", Token.c_str());
						value = ParseExpression();
						if (value.Type != V_INT)
							Fail("Expecting integer constant, found %s.", type2string(value.Type));
						reg.Type = (regType)value.uValue;
						switch (NextToken())
						{default:
							Fail("Expected ',' or ']', found '%s'.", Token.c_str());
						 case COMMA:
							value = ParseExpression();
							if (value.Type != V_INT)
								Fail("Expecting integer constant, found %s.", type2string(value.Type));
							reg.Rotate = (uint8_t)value.uValue;
							if (NextToken() != SQBRC2)
								Fail("Expected ']', found '%s'.", Token.c_str());
							break;
						 case SQBRC2:
							reg.Rotate = 0;
						}
						value = reg;
						goto have_value;
					}
				 case NUM:
					// NextToken accepts some letters in numeric constants
					if (Token.back() == 'f') // forward reference
					{	forward = true;
						Token.erase(Token.size()-1);
					}
				 case WORD:;
				}
				const auto& l = LabelsByName.emplace(Token, LabelCount);
				if (l.second || (forward && Labels[l.first->second].Definition))
				{	// new label
					l.first->second = LabelCount;
					if (!Pass2)
						Labels.emplace_back(Token);
					else if (Labels.size() <= LabelCount || Labels[LabelCount].Name != Token)
						Fail("Inconsistent label definition during Pass 2.");
					Labels[LabelCount].Reference = *Context.back();
					++LabelCount;
				}
				value = exprValue(Labels[l.first->second].Value, V_LABEL);
				break;
			}

		 default:
		 discard:
			At -= Token.size();
			return eval.Evaluate();

		 case OP:
		 case BRACE1:
		 case BRACE2:
			{	const opInfo* op = binary_search(operatorMap, Token.c_str());
				if (!op)
					Fail("Invalid operator: %s", Token.c_str());
				if (!eval.PushOperator(op->Op))
					goto discard;
				goto next;
			}
		 case SQBRC1: // per QPU element constant
			value = parseElemInt();
			break;

		 case NUM:
			// parse number
			if (Token.find('.') == string::npos)
			{	// integer
				//size_t len;  sscanf of gcc4.8.2/Linux x32 can't read "0x80000000".
				//if (sscanf(Token.c_str(), "%i%n", &stack.front().iValue, &len) != 1 || len != Token.size())
				if (parseUInt(Token.c_str(), value.uValue) != Token.size())
					Fail("%s is no integral number.", Token.c_str());
				value.Type = V_INT;
			} else
			{	// float number
				size_t len;
				if (sscanf(Token.c_str(), "%f%n", &value.fValue, &len) != 1 || len != Token.size())
					Fail("%s is no real number.", Token.c_str());
				value.Type = V_FLOAT;
			}
			break;
		}
	 have_value:
		eval.PushValue(value);
		goto next;
	} catch (const string& msg)
	{	throw enrichMsg(msg);
	}
}

Inst::mux Parser::muxReg(reg_t reg)
{	if (reg.Type & R_SEMA)
		Fail("Cannot use semaphore source in ALU instruction.");
	if (!(reg.Type & R_READ))
	{	// direct read access for r0..r5.
		if ((reg.Num ^ 32U) <= 5U)
			return (Inst::mux)(reg.Num ^ 32);
		Fail("The register is not readable.");
	}
	// try RA
	if (reg.Type & R_A)
	{	if ( Instruct.RAddrA == reg.Num
			|| ( (Instruct.OpA == Inst::A_NOP || (Instruct.MuxAA != Inst::X_RA && Instruct.MuxAB != Inst::X_RA))
				&& (Instruct.OpM == Inst::M_NOP || (Instruct.MuxMA != Inst::X_RA && Instruct.MuxMB != Inst::X_RA)) ))
		{	Instruct.RAddrA = reg.Num;
			return Inst::X_RA;
		}
	}
	// try RB
	if (reg.Type & R_B)
	{	if (Instruct.Sig >= Inst::S_SMI)
			Fail("Access to register file B conflicts with small immediate value.");
		if ( Instruct.RAddrB == reg.Num
			|| ( (Instruct.OpA == Inst::A_NOP || (Instruct.MuxAA != Inst::X_RB && Instruct.MuxAB != Inst::X_RB))
				&& (Instruct.OpM == Inst::M_NOP || (Instruct.MuxMA != Inst::X_RB && Instruct.MuxMB != Inst::X_RB)) ))
		{	Instruct.RAddrB = reg.Num;
			return Inst::X_RB;
		}
	}
	Fail("Read access to register conflicts with another access to the same register file.");
}

uint8_t Parser::getSmallImmediate(uint32_t i)
{	if (i + 16 <= 0x1f)
		return (uint8_t)(i & 0x1f);
	if (!((i - 0x3b800000) & 0xf87fffff))
		return (uint8_t)(((i >> 23) - 0x77) ^ 0x28);
	return 0xff; // failed
}

const Parser::smiEntry* Parser::getSmallImmediateALU(uint32_t i)
{	size_t l = 0;
	size_t r = sizeof smiMap / sizeof *smiMap - 1;
	while (l < r)
	{	size_t m = (l+r) >> 1;
		if (i <= smiMap[m].Value)
			r = m;
		else
			l = m + 1;
	}
	return smiMap + r;
}

void Parser::doSMI(uint8_t si)
{	switch (Instruct.Sig)
	{default:
		Fail("Small immediate value or vector rotation cannot be used together with signals.");
	 case Inst::S_SMI:
		if (Instruct.SImmd != si)
			Fail("Only one distinct small immediate value supported per instruction. Requested value: %u, current Value: %u.", si, Instruct.SImmd);
		return; // value hit
	 case Inst::S_NONE:
		if (Instruct.RAddrB != Inst::R_NOP )
			Fail("Small immediate cannot be used together with register file B read access.");
	}
	Instruct.Sig   = Inst::S_SMI;
	Instruct.SImmd = si;
}

void Parser::addIf(int cond, InstContext ctx)
{
	if (Instruct.Sig == Inst::S_BRANCH)
		Fail("Cannot apply conditional store (.ifxx) to branch instruction.");
	auto& target = ctx & IC_MUL ? Instruct.CondM : Instruct.CondA;
	if (target != Inst::C_AL)
		Fail("Store condition (.if) already specified.");
	target = (Inst::conda)cond;
}

void Parser::addUnpack(int mode, InstContext ctx)
{
	if (Instruct.Sig >= Inst::S_LDI)
		Fail("Cannot apply .unpack to branch and load immediate instructions.");
	if (Instruct.Unpack != Inst::U_32 && Instruct.Unpack != mode)
		Fail("Only one distinct .unpack per instruction, please.");
	Instruct.Unpack = (Inst::unpack)mode;
}

void Parser::addPack(int mode, InstContext ctx)
{
	if (Instruct.Sig == Inst::S_BRANCH)
		Fail("Cannot apply .pack to branch instruction.");
	if (Instruct.Pack != Inst::P_32 && Instruct.Pack != mode)
		Fail("Only one distinct .pack per instruction, please.");
	Instruct.Pack = (Inst::pack)mode;
}

void Parser::addSetF(int, InstContext ctx)
{
	if (Instruct.Sig == Inst::S_BRANCH)
		Fail("Cannot apply .setf to branch instruction.");
	if ( Instruct.Sig != Inst::S_LDI && (ctx & IC_MUL)
		&& (Instruct.WAddrA != Inst::R_NOP || Instruct.OpA != Inst::A_NOP) )
		Fail("Cannot apply .setf because the flags of the ADD ALU will be used.");
	if (Instruct.SF)
		Fail("Don't use .setf twice.");
	Instruct.SF = true;
}

void Parser::addCond(int cond, InstContext ctx)
{
	if (Instruct.Sig != Inst::S_BRANCH)
		Fail("Branch condition codes can only be applied to branch instructions.");
	if (Instruct.CondBr != Inst::B_AL)
		Fail("Only one branch condition per instruction, please.");
	Instruct.CondBr = (Inst::condb)cond;
}

void Parser::addRot(int, InstContext ctx)
{
	if ((ctx & IC_MUL) == 0)
		Fail("QPU element rotation is only available with the MUL ALU.");
	auto count = ParseExpression();
	if (NextToken() != COMMA)
		Fail("Expected ',' after rotation count.");

	uint8_t si = 48;
	switch (count.Type)
	{case V_REG:
		if ((count.rValue.Type & R_READ) && count.rValue.Num == 37) // r5
			break;
	 default:
		Fail("QPU rotation needs an integer argument or r5 for the rotation count.");
	 case V_INT:
		si += count.uValue & 0xf;
		if (si == 48)
			return; // Rotation is a multiple of 16 => nothing to do
	}

	doSMI(si);
}

void Parser::doInstrExt(InstContext ctx)
{
	while (NextToken() == DOT)
	{	if (NextToken() != WORD)
			Fail("Expected instruction extension after dot.");

		const opExtEntry* ep = binary_search(extMap, Token.c_str());
		if (!ep)
		 fail:
			Fail("Unknown instruction extension '%s' within this context.", Token.c_str());
		opExtFlags filter = ctx & IC_SRC ? E_SRC : ctx & IC_DST ? E_DST : E_OP;
		while ((ep->Flags & filter) == 0)
			if (strcmp(Token.c_str(), (++ep)->Name) != 0)
				goto fail;
		(this->*(ep->Func))(ep->Arg, ctx);
	}
	At -= Token.size();
}

void Parser::doALUTarget(exprValue param, bool mul)
{	if (param.Type != V_REG)
		Fail("The first argument to a ALU or branch instruction must be a register or '-', found %s.", Token.c_str());
	if (!(param.rValue.Type & R_WRITE))
		Fail("The register is not writable.");
	if ((param.rValue.Type & R_AB) != R_AB)
	{	bool wsfreeze = Instruct.Pack != Inst::P_32 || mul
			? Instruct.OpA != Inst::A_NOP && !Inst::isWRegAB(Instruct.WAddrA)
			: Instruct.OpM != Inst::M_NOP && !Inst::isWRegAB(Instruct.WAddrM);
		if ((param.rValue.Type & R_A) && (!wsfreeze || Instruct.WS == mul))
			Instruct.WS = mul;
		else if ((param.rValue.Type & R_B) && (!wsfreeze || Instruct.WS != mul))
			Instruct.WS = !mul;
		else
			Fail("ADD ALU and MUL ALU cannot write to the same register file.");
	}
	(mul ? Instruct.WAddrM : Instruct.WAddrA) = param.rValue.Num;
	// vector rotation
	if (param.rValue.Rotate)
	{	if (!mul)
			Fail("Vector rotation is only available to the MUL ALU.");
		if (param.rValue.Rotate == 16)
			Fail("Can only rotate ALU target left by r5.");
		doSMI(48 + (param.rValue.Rotate & 0xf));
	}

	doInstrExt((InstContext)(mul*IC_MUL | IC_DST));
}

Inst::mux Parser::doALUExpr(InstContext ctx)
{	if (NextToken() != COMMA)
		Fail("Expected ',' before next argument to ALU instruction, found %s.", Token.c_str());
	exprValue param = ParseExpression();
	switch (param.Type)
	{default:
		Fail("The second argument of a binary ALU instruction must be a register or a small immediate value.");
	 case V_REG:
		{	auto ret = muxReg(param.rValue);
			if (param.rValue.Rotate)
			{	if ((ctx & IC_MUL) == 0)
					Fail("Vector rotation is only available to the MUL ALU.");
				if (param.rValue.Rotate == -16)
					Fail("Can only rotate ALU source right by r5.");
				if (Instruct.Sig == Inst::S_SMI && Instruct.SImmd >= 48)
					Fail("Vector rotation is already applied to the instruction.");
				doSMI(48 + (-param.rValue.Rotate & 0xf));
			}
			return ret;
		}
	 case V_FLOAT:
	 case V_INT:
		{	// some special hacks for ADD ALU
			if (ctx == IC_SRCB)
			{	switch (Instruct.OpA)
				{case Inst::A_ADD: // swap ADD and SUB in case of constant 16
				 case Inst::A_SUB:
					if (param.uValue == 16)
					{	(uint8_t&)Instruct.OpA ^= Inst::A_ADD ^ Inst::A_SUB; // swap add <-> sub
						param.iValue = -16;
					}
					break;
				 case Inst::A_ASR: // shift instructions ignore all bit except for the last 5
				 case Inst::A_SHL:
				 case Inst::A_SHR:
				 case Inst::A_ROR:
					param.iValue = (param.iValue << 27) >> 27; // sign extend
				 default:;
				}
			}
			uint8_t si = getSmallImmediate(param.uValue);
			if (si == 0xff)
				Fail("Value 0x%x does not fit into the small immediate field.", param.uValue);
			doSMI(si);
			return Inst::X_RB;
		}
	}

	doInstrExt(ctx);
}

void Parser::doBRASource(exprValue param)
{
	switch (param.Type)
	{default:
		Fail("Data type is not allowed as branch target.");
	 case V_LABEL:
		if (Instruct.Rel)
			param.uValue -= (PC + 4) * sizeof(uint64_t);
		else
			Msg(WARNING, "Using value of label as target of a absolute branch instruction.");
	 case V_INT:
		if (!param.uValue)
			return;
		if (Instruct.Immd.uValue)
			Fail("Cannot specify two immediate values as branch target.");
		Instruct.Immd = param;
		break;
	 case V_REG:
		if (!(param.rValue.Type & R_WRITE))
			Fail("Branch target register is not writable.");
		if (param.rValue.Num == Inst::R_NOP && !param.rValue.Rotate && (param.rValue.Type & R_AB))
			return;
		if (!(param.rValue.Type & R_A) || param.rValue.Num >= 32)
			Fail("Branch target must be from register file A and no hardware register.");
		if (param.rValue.Rotate)
			Fail("Cannot use vector rotation with branch instruction.");
		if (Instruct.Reg)
			Fail("Cannot specify two registers as branch target.");
		Instruct.Reg = true;
		Instruct.RAddrA = param.rValue.Num;
		break;
	}
}

void Parser::assembleADD(int add_op)
{
	if (Instruct.Sig >= Inst::S_LDI)
		Fail("Cannot use ADD ALU in load immediate or branch instruction.");
	if (Instruct.WAddrA != Inst::R_NOP || Instruct.OpA != Inst::A_NOP)
	{	switch (add_op)
		{default:
			Fail("The ADD ALU has already been used in this instruction.");
		 case Inst::A_NOP:
			add_op = Inst::M_NOP; break;
		 case Inst::A_V8ADDS:
			add_op = Inst::M_V8ADDS; break;
		 case Inst::A_V8SUBS:
			add_op = Inst::M_V8SUBS; break;
		}
		// retry with MUL ALU
		return assembleMUL(add_op | 0x100); // The added numbed is discarded at the cast but avoids recursive retry.
	}

	Instruct.OpA = (Inst::opadd)add_op;
	if (add_op == Inst::A_NOP)
	{	Flags() |= IF_HAVE_NOP;
		return;
	}

	doInstrExt(IC_NONE);

	doALUTarget(ParseExpression(), false);

	if (!Instruct.isUnary())
		Instruct.MuxAA = doALUExpr(IC_SRC);

	Instruct.MuxAB = doALUExpr(IC_SRCB);
}

void Parser::assembleMUL(int mul_op)
{
	if (Instruct.Sig >= Inst::S_LDI)
		Fail("Cannot use MUL ALU in load immediate or branch instruction.");
	if (Instruct.WAddrM != Inst::R_NOP || Instruct.OpM != Inst::M_NOP)
	{	switch (mul_op)
		{default:
			Fail("The MUL ALU has already been used by the current instruction.");
		 case Inst::M_NOP:
			mul_op = Inst::A_NOP; break;
		 case Inst::M_V8ADDS:
			mul_op = Inst::A_V8ADDS; break;
		 case Inst::M_V8SUBS:
			mul_op = Inst::A_V8SUBS; break;
		}
		// retry with MUL ALU
		return assembleADD(mul_op | 0x100); // The added numbed is discarded at the cast but avoids recursive retry.
	}

	Instruct.OpM = (Inst::opmul)mul_op;
	if (mul_op == Inst::M_NOP)
		return;

	doInstrExt(IC_MUL);

	doALUTarget(ParseExpression(), true);

	Instruct.MuxMA = doALUExpr(IC_MULSRC);
	Instruct.MuxMB = doALUExpr(IC_MULSRCB);
}

void Parser::assembleMOV(int mode)
{
	if (Instruct.Sig == Inst::S_BRANCH)
		Fail("Cannot use MOV together with branch instruction.");
	bool isLDI = Instruct.Sig == Inst::S_LDI;
	bool useMUL = (Flags() & IF_HAVE_NOP) || Instruct.WAddrA != Inst::R_NOP || (!isLDI && Instruct.OpA != Inst::A_NOP);
	if (useMUL && (Instruct.WAddrM != Inst::R_NOP || (!isLDI && Instruct.OpM != Inst::M_NOP)))
		Fail("Both ALUs are already used by the current instruction.");

	doInstrExt((InstContext)(useMUL * IC_MUL));

	doALUTarget(ParseExpression(), useMUL);

	if (NextToken() != COMMA)
		Fail("Expected ', <source1>' after first argument to ALU instruction, found %s.", Token.c_str());
	exprValue param = ParseExpression();
	switch (param.Type)
	{default:
		Fail("The last parameter of a MOV instruction must be a register or a immediate value. Found %s", type2string(param.Type));
	 case V_REG:
		if (param.rValue.Type & R_SEMA)
		{	// semaphore access by LDI like instruction
			mode = Inst::L_SEMA;
			param.uValue = param.rValue.Num | (param.rValue.Type & R_SACQ);
			break;
		}
		if (isLDI)
			Fail("mov instruction with register source cannot be combined with load immediate.");
		if (param.rValue.Rotate)
		{	if (!useMUL)
				Fail("Vector rotation is only available to the MUL ALU.");
			if (param.rValue.Rotate == 16)
				Fail("Can only rotate ALU source left by r5.");
			if (Instruct.Sig == Inst::S_SMI && Instruct.SImmd >= 48)
				Fail("Vector rotation is already active on this instruction.");
			doSMI(48 + (-param.rValue.Rotate & 0xf));
		}
		if (useMUL)
		{	Instruct.MuxMA = Instruct.MuxMB = muxReg(param.rValue);
			Instruct.OpM = Inst::M_V8MIN;
		} else
		{	Instruct.MuxAA = Instruct.MuxAB = muxReg(param.rValue);
			Instruct.OpA = Inst::A_OR;
		}
		return;
	 case V_LDPES:
		if (mode != Inst::L_LDI && mode != Inst::L_PES)
			Fail("Load immediate mode conflicts with per QPU element constant.");
		mode = Inst::L_PES;
		break;
	 case V_LDPEU:
		if (mode != Inst::L_LDI && mode != Inst::L_PEU)
			Fail("Load immediate mode conflicts with per QPU element constant.");
		mode = Inst::L_PEU;
		break;
	 case V_LDPE:
		if (mode >= Inst::L_SEMA)
			Fail("Load immediate mode conflicts with per QPU element constant.");
		if (mode < Inst::L_PES)
			mode = Inst::L_PES;
		break;
	 case V_INT:
	 case V_FLOAT:
		// try small immediate first
		if (!isLDI && mode < 0)
		{	switch (Instruct.Sig)
			{default:
				Fail ("Immediate values cannot be used together with signals.");
			 case Inst::S_NONE:
				if (Instruct.RAddrB != Inst::R_NOP)
					Fail ("Immediate value collides with read from register file B.");
			 case Inst::S_SMI:;
			}
			for (const smiEntry* si = getSmallImmediateALU(param.uValue); si->Value == param.uValue; ++si)
			{	if ( (!si->OpCode.isMul() ^ useMUL)
					&& (Extensions || !si->OpCode.isExt())
					&& ( param.uValue == 0 || Instruct.Sig == Inst::S_NONE
						|| (Instruct.Sig == Inst::S_SMI && Instruct.SImmd == si->SImmd) ))
				{	if (param.uValue != 0) // zero value does not require SMI
					{	Instruct.Sig   = Inst::S_SMI;
						Instruct.SImmd = si->SImmd;
					}
					if (si->OpCode.isMul())
					{	Instruct.MuxMA = Instruct.MuxMB = Inst::X_RB;
						Instruct.OpM   = si->OpCode.asMul();
					} else
					{	Instruct.MuxAA = Instruct.MuxAB = Inst::X_RB;
						Instruct.OpA   = si->OpCode.asAdd();
					}
					return;
				}
			}
		}
		// LDI
		if (mode < 0)
			mode = Inst::L_LDI;
	}
	switch (Instruct.Sig)
	{default:
		Fail("Load immediate cannot be used with signals.");
	 case Inst::S_LDI:
		if (Instruct.Immd.uValue != param.uValue || Instruct.LdMode != mode)
			Fail("Tried to load two different immediate values in one instruction. (0x%x vs. 0x%x)", Instruct.Immd.uValue, param.uValue);
	 case Inst::S_NONE:;
	}
	// LDI or semaphore
	if (Instruct.OpA != Inst::A_NOP || Instruct.OpM != Inst::M_NOP)
		Fail("Cannot combine load immediate with value %s with ALU instructions.", param.toString().c_str());
	Instruct.Sig = Inst::S_LDI;
	Instruct.LdMode = (Inst::ldmode)mode;
	Instruct.Immd = param;
}

void Parser::assembleBRANCH(int relative)
{
	if (!Instruct.isVirgin())
		Fail("A branch instruction must be the only one in a line.");

	Instruct.Sig = Inst::S_BRANCH;
	Instruct.CondBr = Inst::B_AL;
	Instruct.Rel = !!relative;
	Instruct.RAddrA = 0;
	Instruct.Reg = false;
	Instruct.Immd.uValue = 0;

	doInstrExt(IC_NONE);

	doALUTarget(ParseExpression(), false);

	if (NextToken() != COMMA)
		Fail("Expected ', <branch target>' after first argument to branch instruction, found %s.", Token.c_str());
	auto param2 = ParseExpression();
	switch (NextToken())
	{default:
		Fail("Expected ',' or end of line, found '%s'.", Token.c_str());
	 case END:
		doBRASource(param2);
		break;
	 case COMMA:
		auto param3 = ParseExpression();
		switch (NextToken())
		{default:
			Fail("Expected ',' or end of line, found '%s'.", Token.c_str());
		 case END: // we have 3 arguments => #2 and #3 are branch target
			doBRASource(param2);
			doBRASource(param3);
			break;
		 case COMMA: // we have 4 arguments, so #2 is a target and #3 the first source
			doALUTarget(param2, true);
			doBRASource(param3);
			doBRASource(ParseExpression());
			if (NextToken() != END)
				Fail("Expected end of line after branch instruction.");
		}
	}

	// add branch target flag for the branch point unless the target is 0
	if (Instruct.Reg || Instruct.Immd.uValue != 0)
	{	size_t pos = PC + 4;
		FlagsSize(pos + 1);
		InstFlags[pos] |= IF_BRANCH_TARGET;
	}
}

void Parser::assembleSEMA(int type)
{
	doALUTarget(ParseExpression(), false);
	if (NextToken() != COMMA)
		Fail("Expected ', <number>' after first argument to semaphore instruction, found %s.", Token.c_str());

	auto param = ParseExpression();
	if (param.Type != V_INT || (param.uValue & ~(type << 4)) >= 16)
		Fail("Semaphore instructions require a single integer argument less than 16 with the semaphore number.");
	param.uValue |= type << 4;

	switch (Instruct.Sig)
	{case Inst::S_LDI:
		if (Instruct.LdMode == Inst::L_LDI)
		{	if ((Instruct.Immd.uValue & 0x1f) != param.uValue)
				Fail("Combining a semaphore instruction with load immediate requires the low order 5 bits to match the semaphore number and the direction bit.");
			break;
		}
	 default:
		Fail("Semaphore Instructions normally cannot be combined with any other instruction.");
	 case Inst::S_NONE:
		Instruct.Sig = Inst::S_LDI;
		Instruct.Immd = param;
	}
	Instruct.LdMode = Inst::L_SEMA;
}

void Parser::assembleSIG(int bits)
{
	switch (Instruct.Sig)
	{default:
		Fail("You must not use more than one signaling flag per line.");
	 case Inst::S_BRANCH:
	 case Inst::S_LDI:
	 case Inst::S_SMI:
		Fail("Signaling bits cannot be combined with branch instruction or immediate values.");
	 case Inst::S_NONE:
		Instruct.Sig = (Inst::sig)bits;
	}
}

void Parser::ParseInstruction()
{
	auto& flags = Flags();
	while (true)
	{	flags &= ~IF_CMB_ALLOWED;
		const opEntry<8>* op = binary_search(opcodeMap, Token.c_str());
		if (!op)
			Fail("Invalid opcode or unknown macro: %s", Token.c_str());

		if (Preprocessed)
			fputs(Token.c_str(), Preprocessed);
		(this->*op->Func)(op->Arg);

		switch (NextToken())
		{default:
			Fail("Expected end of line ore ';' after instruction. Found '%s'.", Token.c_str());
		 case END:
			return;
		 case SEMI:
			flags |= IF_CMB_ALLOWED;
			switch (NextToken())
			{default:
				Fail("Expected additional instruction or end of line after ';'. Found '%s'.", Token.c_str());
			 case END:
				return;
			 case WORD:;
			}
		}
	}
}

void Parser::defineLabel()
{
	// Lookup symbol
	const auto& lname = LabelsByName.emplace(Token, LabelCount);
	label* lp;
	if (lname.second)
	{	// new label, not yet referenced
	 new_label:
		if (!Pass2)
			Labels.emplace_back(Token);
		lp = &Labels[LabelCount];
		++LabelCount;
	} else
	{	// Label already in the lookup table.
		lp = &Labels[lname.first->second];
		if (lp->Definition)
		{	// redefinition
			if (!isdigit(lp->Name[0]))
				Fail("Redefinition of non-local label %s, previously defined at %s.",
					Token.c_str(), lp->Definition.toString().c_str());
			// redefinition allowed, but this is always a new label
			lname.first->second = LabelCount;
			goto new_label;
		}
	}
	if (!Pass2)
		lp->Value = PC * sizeof(uint64_t);
	else if (lp->Name != Token || lp->Value != PC * sizeof(uint64_t))
		Fail("Inconsistent Label definition during Pass 2.");
	lp->Definition = *Context.back();

	if (Preprocessed)
	{	fputs(Token.c_str(), Preprocessed);
		fputs(": ", Preprocessed);
	}
}

void Parser::parseLabel()
{
	switch (NextToken())
	{default:
		Fail("Expected label name after ':', found '%s'.", Token.c_str());
	 case WORD:
	 case NUM:
		defineLabel();
	 case END:
		Flags() |= IF_BRANCH_TARGET;
	}
}

void Parser::parseDATA(int type)
{ int count = 0;
	uint64_t target = 0;
 next:
	exprValue value = ParseExpression();
	if (value.Type != V_INT && value.Type != V_FLOAT)
		Fail("Immediate data instructions require integer or floating point constants. Found %s.", type2string(value.Type));
	switch (type)
	{case -4:
		if (value.Type == V_INT)
			value.fValue = value.iValue;
		type = 4;
		break;
	 case 1: // byte
		if (value.iValue > 0xFF || value.iValue < -0x80)
			Msg(WARNING, "Byte value out of range: 0x%x", value.uValue);
		break;
	 case 2: // short
		if (value.iValue > 0xFFFF || value.iValue < -0x8000)
			Msg(WARNING, "Short integer value out of range: 0x%x", value.uValue);
	}
	// Prevent optimizer across .data segment
	Flags() |= IF_BRANCH_TARGET;
	// store value
	target |= (uint64_t)value.uValue << count;
	if ((count += (type << 3)) >= 64)
	{	StoreInstruction(target);
		count = 0;
		target = 0;
	}
	switch (NextToken())
	{default:
		Fail("Syntax error. Expected ',' or end of line.");
	 case COMMA:
		goto next;
	 case END:;
	}
	if (count & 63)
	{	Msg(INFO, "Used padding to enforce 64 bit alignment of immediate data.");
		StoreInstruction(target);
	}
	// Prevent optimizer across .data segment
	FlagsSize(PC + 1);
	Flags() |= IF_BRANCH_TARGET;
	Instruct.reset();
}

void Parser::beginREP(int)
{
	if (doPreprocessor())
		return;

	AtMacro = &Macros[".rep"];
	AtMacro->Definition = *Context.back();

	if (NextToken() != WORD)
		Fail("Expected loop variable name after .rep.");

	AtMacro->Args.push_back(Token);
	if (NextToken() != COMMA)
		Fail("Expected ', <count>' at .rep.");
	const auto& expr = ParseExpression();
	if (expr.Type != V_INT)
		Fail("Second argument to .rep must be an integral number. Found %s", expr.toString().c_str());
	if (NextToken() != END)
		Fail("Expected end of line.");
	AtMacro->Args.push_back(expr.toString());
}

void Parser::endREP(int)
{
	auto iter = Macros.find(".rep");
	if (AtMacro != &iter->second)
	{	if (doPreprocessor())
			return;
		Fail(".endr without .rep");
	}

	const macro m = *AtMacro;
	AtMacro = NULL;
	Macros.erase(iter);

	if (m.Args.size() != 2)
		return; // no loop count => 0

	// Setup invocation context
	saveContext ctx(*this, new fileContext(CTX_MACRO, m.Definition.File, m.Definition.Line));

	// loop
	uint32_t count;
	sscanf(m.Args[1].c_str(), "%i", &count);
	auto& current = *Context.back();
	for (uint32_t& i = current.Consts.emplace(m.Args.front(), constDef(exprValue(0), current)).first->second.Value.uValue;
		i < count; ++i)
	{	// Invoke rep
		for (const string& line : m.Content)
		{	++Context.back()->Line;
			strncpy(Line, line.c_str(), sizeof(Line));
			ParseLine();
		}
	}
}

void Parser::beginBACK(int)
{
	if (doPreprocessor())
		return;

	if (Back)
		Fail("Cannot nest .back directives.");
	exprValue param = ParseExpression();
	if (param.Type != V_INT)
		Fail("Expected integer constant after .back.");
	if (param.uValue > 10)
		Fail("Cannot move instructions more than 10 slots back.");
	if (param.uValue > PC)
		Fail("Cannot move instructions back before the start of the code.");
	if (NextToken() != END)
		Fail("Expected end of line, found '%s'.", Token.c_str());
	Back = param.uValue;
	size_t pos = PC -= Back;
	// Load last instruction before .back to provide combine support
	if (pos)
		Instruct.decode(Instructions[pos-1]);

	if (Pass2)
		while (++pos < PC + Back)
			if (InstFlags[pos] & IF_BRANCH_TARGET)
				Msg(WARNING, ".back crosses branch target at address 0x%x. Code might not work.", pos*8);
}

void Parser::endBACK(int)
{
	if (doPreprocessor())
		return;

	/* avoids .back 0  if (!Back)
		Fail(".endb without .back.");*/
	if (NextToken() != END)
		Fail("Expected end of line, found '%s'.", Token.c_str());
	PC += Back;
	Back = 0;
	// Restore last instruction to provide combine support
	if (PC)
		Instruct.decode(Instructions[PC-1]);
}

void Parser::parseCLONE(int)
{
	if (doPreprocessor())
		return;

	exprValue param1 = ParseExpression();
	if (param1.Type != V_LABEL)
		Fail("The first argument to .clone must by a label. Found %s.", type2string(param1.Type));
	param1.uValue >>= 3; // offset in instructions rather than bytes
	if (NextToken() != COMMA)
		Fail("Expected ', <count>' at .clone.");
	exprValue param2 = ParseExpression();
	if (param2.Type != V_INT || param2.uValue > 3)
		Fail("Expected integer constant in the range [0,3].");
	FlagsSize(PC + param2.uValue);
	param2.uValue += param1.uValue; // end offset rather than count
	if (Pass2 && param2.uValue >= Instructions.size())
		Fail("TODO: Cannot clone behind the end of the code.");

	for (size_t src = param1.uValue; src < param2.uValue; ++src)
	{	InstFlags[PC] |= InstFlags[src] & ~IF_BRANCH_TARGET;
		if ((Instructions[src] & 0xF000000000000000ULL) == 0xF000000000000000ULL)
			Msg(WARNING, "You should not clone branch instructions. (#%u)", src - param1.uValue);
		StoreInstruction(Instructions[src]);
	}
	// Restore last instruction to provide combine support
	if (PC)
		Instruct.decode(Instructions[PC-1]);
}

void Parser::parseSET(int flags)
{
	if (doPreprocessor())
		return;

	if (NextToken() != WORD)
		Fail("Directive .set requires identifier.");
	string name = Token;
	switch (NextToken())
	{default:
		Fail("Directive .set requires ', <value>' or '(<arguments>) <value>'. Fount %s.", Token.c_str());
	 case BRACE1:
		{	function func(*Context.back());
		 next:
			if (NextToken() != WORD)
				Fail("Function argument name expected. Found '%s'.", Token.c_str());
			func.Args.push_back(Token);
			switch (NextToken())
			{default:
				Fail("Expected ',' or ')' after function argument.");
			 case NUM:
				Fail("Function arguments must not start with a digit.");
			 case COMMA:
				goto next;
			 case END:
				Fail("Unexpected end of function argument definition");
			 case BRACE2:
				break;
			}

			// Anything after ')' is function body and evaluated delayed
			At += strspn(At, " \t\r\n,");
			func.DefLine = Line;
			func.Start = At;

			const auto& ret = Functions.emplace(name, func);
			if (!ret.second)
			{	Msg(INFO, "Redefinition of function %s.\n"
				      "Previous definition at %s.",
				  Token.c_str(), ret.first->second.Definition.toString().c_str());
				ret.first->second = func;
			}
			break;
		}
	 case COMMA:
		{	exprValue expr = ParseExpression();
			if (NextToken() != END)
				Fail("Syntax error: unexpected %s.", Token.c_str());

			auto& current = flags & C_LOCAL ? *Context.back() : *Context.front();
			auto r = current.Consts.emplace(name, constDef(expr, current));
			if (!r.second)
			{	if (flags & C_CONST)
					// redefinition not allowed
					Fail("Identifier %s has already been defined at %s.",
						name.c_str(), r.first->second.Definition.toString().c_str());
				r.first->second.Value = expr;
			}
		}
	}
}

void Parser::parseUNSET(int flags)
{
	if (doPreprocessor())
		return;

	if (NextToken() != WORD)
		Fail("Directive .unset requires identifier.");
	string Name = Token;
	if (NextToken() != END)
		Fail("Syntax error: unexpected %s.", Token.c_str());

	auto& consts = (flags & C_LOCAL ? Context.back() : Context.front())->Consts;
	auto r = consts.find(Name);
	if (r == consts.end())
		return Msg(WARNING, "Cannot unset %s because it has not yet been definied in the required context.", Name.c_str());
	consts.erase(r);
}

bool Parser::doCondition()
{
	const exprValue& param = ParseExpression();
	if (param.Type != V_INT)
		Fail("Conditional expression must be a integer constant, found '%s'.", param.toString().c_str());
	if (NextToken() != END)
		Fail("Expected end of line, found '%s'.", Token.c_str());
	return param.uValue != 0;
}

void Parser::parseIF(int)
{
	if (doPreprocessor(PP_MACRO))
		return;

	AtIf.emplace_back(Context.back()->Line, isDisabled() ? 4 : doCondition());
}

void Parser::parseELSEIF(int)
{
	if (doPreprocessor(PP_MACRO))
		return;

	if (!AtIf.size())
		Fail(".elseif without .if");

	if (AtIf.back().State == 0)
		AtIf.back().State = doCondition();
	else
		AtIf.back().State |= 2;
}

void Parser::parseELSE(int)
{
	if (doPreprocessor(PP_MACRO))
		return;

	if (!AtIf.size())
		Fail(".else without .if");
	if (NextToken() != END)
		Msg(ERROR, "Expected end of line. .else has no arguments.");

	++AtIf.back().State;
}

void Parser::parseENDIF(int)
{
	if (doPreprocessor(PP_MACRO))
		return;

	if (!AtIf.size())
		Fail(".endif without .if");
	if (NextToken() != END)
		Msg(ERROR, "Expected end of line. .endif has no arguments.");

	AtIf.pop_back();
}

void Parser::parseASSERT(int)
{
	if (doPreprocessor())
		return;

	if (!doCondition())
		Msg(ERROR, "Assertion failed.");
}

void Parser::beginMACRO(int flags)
{
	if (doPreprocessor(PP_IF))
		return;

	if (AtMacro)
		Fail("Cannot nest macro definitions.\n"
		     "  In definition of macro starting at %s.",
		  AtMacro->Definition.toString().c_str());
	if (NextToken() != WORD)
		Fail("Expected macro name.");
	AtMacro = &(flags & M_FUNC ? MacroFuncs : Macros)[Token];
	if (AtMacro->Definition.File.size())
	{	Msg(INFO, "Redefinition of macro %s.\n"
		          "  Previous definition at %s.",
		  Token.c_str(), AtMacro->Definition.toString().c_str());
		// redefine
		AtMacro->Args.clear();
		AtMacro->Content.clear();
	}
	AtMacro->Definition = *Context.back();
	AtMacro->Flags = (macroFlags)flags;

	int brace = 0;
	while (true)
		switch (NextToken())
		{case BRACE1:
			if (!AtMacro->Args.size())
			{	brace = 1;
				goto comma;
			}
		 default:
			Fail("Expected ',' before (next) macro argument.");
		 case NUM:
			Fail("Macro arguments must not be numbers.");
		 case COMMA:
			if (brace == 2)
				Fail("Expected end of line after closing brace.");
		 comma:
			// Macro argument
			if (NextToken() != WORD)
				Fail("Macro argument name expected. Found '%s'.", Token.c_str());
			AtMacro->Args.push_back(Token);
			break;
		 case BRACE2:
			if (brace != 1)
				Fail("Unexpected closing brace.");
			brace = 2;
			break;
		 case END:
			if (brace == 1)
				Fail("Closing brace is missing");
			return;
		}
}

void Parser::endMACRO(int flags)
{
	if (doPreprocessor(PP_IF))
		return;

	if (!AtMacro)
		Fail(".%s outside a macro definition.", Token.c_str());
	if (AtMacro->Flags != flags)
		Fail("Cannot close this macro with .%s. Expected .end%c.", Token.c_str(), flags & M_FUNC ? 'f' : 'm');
	AtMacro = NULL;
	if (NextToken() != END)
		Msg(ERROR, "Expected end of line.");
}

void Parser::doMACRO(macros_t::const_iterator m)
{
	// Fetch macro arguments
	const auto& argnames = m->second.Args;
	vector<exprValue> args;
	if (argnames.size())
	{	args.reserve(argnames.size());
		while (true)
		{	args.emplace_back(ParseExpression());
			switch (NextToken())
			{default:
				Fail("internal error");
			 case COMMA:
				if (args.size() == argnames.size())
					Fail("Too much arguments for macro %s.", m->first.c_str());
				continue;
			 case END:
				if (args.size() != argnames.size())
					Fail("Too few arguments for macro %s.", m->first.c_str());
			}
			break;
		}
	} else if (NextToken() != END)
		Fail("The macro %s does not take arguments.", m->first.c_str());

	// Setup invocation context
	saveContext ctx(*this, new fileContext(CTX_MACRO, m->second.Definition.File, m->second.Definition.Line));

	// setup args inside new context to avoid interaction with argument values that are also functions.
	auto& current = *Context.back();
	current.Consts.reserve(current.Consts.size() + argnames.size());
	size_t n = 0;
	for (auto arg : argnames)
		current.Consts.emplace(arg, constDef(args[n++], current));

	// Invoke macro
	for (const string& line : m->second.Content)
	{	++Context.back()->Line;
		strncpy(Line, line.c_str(), sizeof(Line));
		ParseLine();
	}
}

exprValue Parser::doFUNCMACRO(macros_t::const_iterator m)
{
	if (NextToken() != BRACE1)
		Fail("Expected '(' after function name.");

	// Fetch macro arguments
	const auto& argnames = m->second.Args;
	vector<exprValue> args;
	args.reserve(argnames.size());
	if (argnames.size() == 0)
	{	// no arguments
		if (NextToken() != BRACE2)
			Fail("Expected ')' because function %s has no arguments.", m->first.c_str());
	} else
	{next:
		args.push_back(ParseExpression());
		switch (NextToken())
		{case BRACE2:
			// End of argument list. Are we complete?
			if (args.size() != argnames.size())
				Fail("Too few arguments for function %s. Expected %u, found %u.", m->first.c_str(), argnames.size(), args.size());
			break;
		 default:
			Fail("Unexpected '%s' in argument list of function %s.", Token.c_str(), m->first.c_str());
		 case COMMA:
			// next argument
			if (args.size() == argnames.size())
				Fail("Too much arguments for function %s. Expected %u.", m->first.c_str(), argnames.size());
			goto next;
		}
	}

	// Setup invocation context
	saveLineContext ctx(*this, new fileContext(CTX_MACRO, m->second.Definition.File, m->second.Definition.Line));

	// setup args inside new context to avoid interaction with argument values that are also functions.
	auto& current = *Context.back();
	current.Consts.reserve(current.Consts.size() + argnames.size());
	size_t n = 0;
	for (auto arg : argnames)
		current.Consts.emplace(arg, constDef(args[n++], current));

	// Invoke macro
	exprValue ret;
	for (const string& line : m->second.Content)
	{	++Context.back()->Line;
		strncpy(Line, line.c_str(), sizeof(Line));
		At = Line;
		switch (NextToken())
		{case DOT:
			// directives
			ParseDirective();
		 case END:
			break;

		 case COLON:
		 label:
			Msg(ERROR, "Label definition not allowed in functional macro %s.", m->first.c_str());
			break;

		 default:
			if (doPreprocessor())
				break;
			// read-ahead to see if the next token is a colon in which case
			// this is a label.
			if (*At == ':')
				goto label;
			if (ret.Type != V_NONE)
			{	Msg(ERROR, "Only one expression allowed per functional macro.");
				break;
			}
			At -= Token.size();
			ret = ParseExpression();
		}
	}
	if (ret.Type == V_NONE)
		Fail("Failed to return a value in functional macro %s.", m->first.c_str());
	return ret;
}

exprValue Parser::doFUNC(funcs_t::const_iterator f)
{
	if (NextToken() != BRACE1)
		Fail("Expected '(' after function name.");

	// Fetch macro arguments
	const auto& argnames = f->second.Args;
	vector<exprValue> args;
	args.reserve(argnames.size());
	if (argnames.size() == 0)
	{	// no arguments
		if (NextToken() != BRACE2)
			Fail("Expected ')' because function %s has no arguments.", f->first.c_str());
	} else
	{next:
		args.push_back(ParseExpression());
		switch (NextToken())
		{case BRACE2:
			// End of argument list. Are we complete?
			if (args.size() != argnames.size())
				Fail("Too few arguments for function %s. Expected %u, found %u.", f->first.c_str(), argnames.size(), args.size());
			break;
		 default:
			Fail("Unexpected '%s' in argument list of function %s.", Token.c_str(), f->first.c_str());
		 case COMMA:
			// next argument
			if (args.size() == argnames.size())
				Fail("Too much arguments for function %s. Expected %u.", f->first.c_str(), argnames.size());
			goto next;
		}
	}

	// Setup invocation context
	saveLineContext ctx(*this, new fileContext(CTX_FUNCTION, f->second.Definition.File, f->second.Definition.Line));
	strncpy(Line, f->second.DefLine.c_str(), sizeof Line);
	At = f->second.Start;
	// setup args inside new context to avoid interaction with argument values that are also functions.
	auto& current = *Context.back();
	current.Consts.reserve(current.Consts.size() + argnames.size());
	unsigned n = 0;
	for (auto arg : argnames)
		current.Consts.emplace(arg, constDef(args[n++], current));

	const exprValue&& val = ParseExpression();
	if (NextToken() != END)
		Fail("Function %s evaluated to an incomplete expression.", f->first.c_str());
	return val;
}

void Parser::doINCLUDE(int)
{
	if (doPreprocessor())
		return;

	At += strspn(At, " \t\r\n");
	{	// remove trailing blanks
		char* cp = At + strlen(At);
		while (strchr(" \t\r\n", *--cp))
			*cp = 0;
	}
	size_t len = strlen(At);
	if (*At != '"' || At[len-1] != '"')
		Fail("Syntax error. Expected \"<file-name>\" after .include, found '%s'.", At);
	Token.assign(At+1, len-2);

	Token = relpath(Context.back()->File, Token);

	saveContext ctx(*this, new fileContext(CTX_INCLUDE, Token, 0));

	ParseFile();
}

void Parser::ParseDirective()
{
	if (NextToken() != WORD)
		Fail("Expected assembler directive after '.'. Found '%s'.", Token.c_str());

	const opEntry<8>* op = binary_search(directiveMap, Token.c_str());
	if (!op)
		Fail("Invalid assembler directive: %s", Token.c_str());

	(this->*op->Func)(op->Arg);
}

bool Parser::doPreprocessor(preprocType type)
{
	if (AtMacro && (type & PP_MACRO))
	{	AtMacro->Content.push_back(Line);
		return true;
	}
	return (type & PP_IF) && isDisabled();
}

void Parser::ParseLine()
{
	At = Line;
	bool trycombine = false;
	bool isinst = false;
	size_t pos = PC;
	FlagsSize(pos + 1);

 next:
	switch (NextToken())
	{case DOT:
		if (isinst)
			goto def;
		// directives
		ParseDirective();
		return;

	 case END:
		*Line = 0;
		doPreprocessor(PP_MACRO);
		return;

	 case COLON:
		if (doPreprocessor())
			return;
		if (isinst)
			goto def;

		parseLabel();
		goto next;

	 case SEMI:
		if (doPreprocessor())
			return;
		if (pos && (InstFlags[pos-1] & IF_CMB_ALLOWED) && (InstFlags[pos] & IF_BRANCH_TARGET) == 0)
			trycombine = true;
		isinst = true;
		goto next;

	 def:
	 default:
		if (!AtMacro || (AtMacro->Flags & M_FUNC) == 0)
			Fail("Syntax error. Unexpected '%s'.", Token.c_str());
	 case WORD:
		if (doPreprocessor())
			return;

		// read-ahead to see if the next token is a colon in which case
		// this is a label.
		if (*At == ':')
		{	defineLabel();
			InstFlags[pos] |= IF_BRANCH_TARGET;
			++At;
			goto next;
		}

		// Try macro
		macros_t::const_iterator m = Macros.find(Token);
		if (m != Macros.end())
		{	doMACRO(m);
			return;
		}

		if (trycombine)
		{	char* atbak = At;
			bool succbak = Success;
			string tokenbak = Token;
			try
			{	// Try to parse into existing instruction.
				ParseInstruction();
				Instructions[pos-1] = Instruct.encode();
				return;
			} catch (const string& msg)
			{	// Combine failed => try new instruction.
				At = atbak;
				Success = succbak;
				Token = tokenbak;
			}
		}
		// new instruction
		Instruct.reset();

		ParseInstruction();
		StoreInstruction(Instruct.encode());
		return;
	}
}

void Parser::ParseFile()
{
	FILE* f = fopen(Context.back()->File.c_str(), "r");
	if (!f)
		Fail("Failed to open file %s.", Context.back()->File.c_str());
	try
	{	while (fgets(Line, sizeof(Line), f))
		{	++Context.back()->Line;
			try
			{	ParseLine();
			} catch (const string& msg)
			{	// recover from errors
				fputs(msgpfx[ERROR], stderr);
				fputs(msg.c_str(), stderr);
				fputc('\n', stderr);
			}

			if (AtMacro && AtMacro->Definition.Line == 0)
				Fail("!!!");
		}
		fclose(f);
	} catch (...)
	{	fclose(f);
		throw;
	}
}
void Parser::ParseFile(const string& file)
{	if (Pass2)
		throw string("Cannot add another file after pass 2 has been entered.");
	saveContext ctx(*this, new fileContext(CTX_INCLUDE, file, 0));
	try
	{	ParseFile();
		Filenames.emplace_back(file);
	} catch (const string& msg)
	{	// recover from errors
		fputs(msgpfx[ERROR], stderr);
		fputs(msg.c_str(), stderr);
		fputc('\n', stderr);
	}
}

void Parser::ResetPass()
{	AtMacro = NULL;
	AtIf.clear();
	Context.clear();
	Context.emplace_back(new fileContext(CTX_ROOT, string(), 0));
	Functions.clear();
	Macros.clear();
	LabelsByName.clear();
	LabelCount = 0;
	InstFlags.clear();
	PC = 0;
	Instruct.reset();
}

void Parser::EnsurePass2()
{
	if (Pass2 || !Success)
		return;

	// enter pass 2
	Pass2 = true;
	ResetPass();

	// Check all labels
	for (auto& label : Labels)
	{	if (!label.Definition)
			Msg(ERROR, "Label '%s' is undefined. Referenced from %s.\n",
				label.Name.c_str(), label.Reference.toString().c_str());
		if (!label.Reference)
			Msg(INFO, "Label '%s' defined at %s is not used.\n",
				label.Name.c_str(), label.Definition.toString().c_str());
		// prepare for next pass
		label.Definition.Line = 0;
	}

	for (auto file : Filenames)
	{	saveContext ctx(*this, new fileContext(CTX_INCLUDE, file, 0));
		ParseFile();
	}

	// Optimize instructions
	for (auto& inst : Instructions)
	{	Inst optimized;
		optimized.decode(inst);
		optimized.optimize();
		inst = optimized.encode();
	}
}

Parser::Parser()
{	Reset();
}

void Parser::Reset()
{ ResetPass();
	Labels.clear();
	Pass2 = false;
	Filenames.clear();
}

const vector<uint64_t>& Parser::GetInstructions()
{
	EnsurePass2();

	return Instructions;
}
