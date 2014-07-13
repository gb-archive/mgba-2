#include "cli-debugger.h"
#include "decoder.h"
#include "parser.h"

#include <signal.h>

#ifdef USE_PTHREADS
#include <pthread.h>
#endif

struct DebugVector {
	struct DebugVector* next;
	enum DVType {
		DV_ERROR_TYPE,
		DV_INT_TYPE,
		DV_CHAR_TYPE
	} type;
	union {
		int32_t intValue;
		const char* charValue;
	};
};

static const char* ERROR_MISSING_ARGS = "Arguments missing";

static struct CLIDebugger* _activeDebugger;

typedef void (DebuggerCommand)(struct CLIDebugger*, struct DebugVector*);

static void _breakInto(struct CLIDebugger*, struct DebugVector*);
static void _continue(struct CLIDebugger*, struct DebugVector*);
static void _disassemble(struct CLIDebugger*, struct DebugVector*);
static void _next(struct CLIDebugger*, struct DebugVector*);
static void _print(struct CLIDebugger*, struct DebugVector*);
static void _printHex(struct CLIDebugger*, struct DebugVector*);
static void _printStatus(struct CLIDebugger*, struct DebugVector*);
static void _quit(struct CLIDebugger*, struct DebugVector*);
static void _readByte(struct CLIDebugger*, struct DebugVector*);
static void _readHalfword(struct CLIDebugger*, struct DebugVector*);
static void _readWord(struct CLIDebugger*, struct DebugVector*);
static void _setBreakpoint(struct CLIDebugger*, struct DebugVector*);
static void _clearBreakpoint(struct CLIDebugger*, struct DebugVector*);
static void _setWatchpoint(struct CLIDebugger*, struct DebugVector*);

static void _breakIntoDefault(int signal);
static void _printLine(struct CLIDebugger* debugger, uint32_t address, enum ExecutionMode mode);

static struct {
	const char* name;
	DebuggerCommand* command;
} _debuggerCommands[] = {
	{ "b", _setBreakpoint },
	{ "break", _setBreakpoint },
	{ "c", _continue },
	{ "continue", _continue },
	{ "d", _clearBreakpoint },
	{ "delete", _clearBreakpoint },
	{ "dis", _disassemble },
	{ "disasm", _disassemble },
	{ "i", _printStatus },
	{ "info", _printStatus },
	{ "n", _next },
	{ "next", _next },
	{ "p", _print },
	{ "p/x", _printHex },
	{ "print", _print },
	{ "print/x", _printHex },
	{ "q", _quit },
	{ "quit", _quit },
	{ "rb", _readByte },
	{ "rh", _readHalfword },
	{ "rw", _readWord },
	{ "status", _printStatus },
	{ "w", _setWatchpoint },
	{ "watch", _setWatchpoint },
	{ "x", _breakInto },
	{ 0, 0 }
};

static inline void _printPSR(union PSR psr) {
	printf("%08X [%c%c%c%c%c%c%c]\n", psr.packed,
		psr.n ? 'N' : '-',
		psr.z ? 'Z' : '-',
		psr.c ? 'C' : '-',
		psr.v ? 'V' : '-',
		psr.i ? 'I' : '-',
		psr.f ? 'F' : '-',
		psr.t ? 'T' : '-');
}

static void _handleDeath(int sig) {
	UNUSED(sig);
	printf("No debugger attached!\n");
}

static void _breakInto(struct CLIDebugger* debugger, struct DebugVector* dv) {
	UNUSED(debugger);
	UNUSED(dv);
	struct sigaction sa, osa;
	sa.sa_handler = _handleDeath;
	sigemptyset(&sa.sa_mask);
	sigaddset(&sa.sa_mask, SIGTRAP);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGTRAP, &sa, &osa);
#ifdef USE_PTHREADS
	pthread_kill(pthread_self(), SIGTRAP);
#else
	kill(getpid(), SIGTRAP);
#endif
	sigaction(SIGTRAP, &osa, 0);
}

static void _continue(struct CLIDebugger* debugger, struct DebugVector* dv) {
	UNUSED(dv);
	debugger->d.state = DEBUGGER_RUNNING;
}

static void _next(struct CLIDebugger* debugger, struct DebugVector* dv) {
	UNUSED(dv);
	ARMRun(debugger->d.cpu);
	_printStatus(debugger, 0);
}

static void _disassemble(struct CLIDebugger* debugger, struct DebugVector* dv) {
	uint32_t address;
	int size;
	int wordSize;
	enum ExecutionMode mode = debugger->d.cpu->executionMode;

	if (mode == MODE_ARM) {
		wordSize = WORD_SIZE_ARM;
	} else {
		wordSize = WORD_SIZE_THUMB;
	}

	if (!dv || dv->type != DV_INT_TYPE) {
		address = debugger->d.cpu->gprs[ARM_PC] - wordSize;
	} else {
		address = dv->intValue;
		dv = dv->next;
	}

	if (!dv || dv->type != DV_INT_TYPE) {
		size = 1;
	} else {
		size = dv->intValue;
		dv = dv->next; // TODO: Check for excess args
	}

	int i;
	for (i = 0; i < size; ++i) {
		_printLine(debugger, address, mode);
		address += wordSize;
	}
}

static void _print(struct CLIDebugger* debugger, struct DebugVector* dv) {
	UNUSED(debugger);
	for ( ; dv; dv = dv->next) {
		printf(" %u", dv->intValue);
	}
	printf("\n");
}

static void _printHex(struct CLIDebugger* debugger, struct DebugVector* dv) {
	UNUSED(debugger);
	for ( ; dv; dv = dv->next) {
		printf(" 0x%08X", dv->intValue);
	}
	printf("\n");
}

static inline void _printLine(struct CLIDebugger* debugger, uint32_t address, enum ExecutionMode mode) {
	char disassembly[48];
	struct ARMInstructionInfo info;
	if (mode == MODE_ARM) {
		uint32_t instruction = debugger->d.cpu->memory.load32(debugger->d.cpu, address, 0);
		ARMDecodeARM(instruction, &info);
		ARMDisassemble(&info, address + WORD_SIZE_ARM * 2, disassembly, sizeof(disassembly));
		printf("%08X: %s\n", instruction, disassembly);
	} else {
		uint16_t instruction = debugger->d.cpu->memory.loadU16(debugger->d.cpu, address, 0);
		ARMDecodeThumb(instruction, &info);
		ARMDisassemble(&info, address + WORD_SIZE_THUMB * 2, disassembly, sizeof(disassembly));
		printf("%04X: %s\n", instruction, disassembly);
	}
}

static void _printStatus(struct CLIDebugger* debugger, struct DebugVector* dv) {
	UNUSED(dv);
	int r;
	for (r = 0; r < 4; ++r) {
		printf("%08X %08X %08X %08X\n",
			debugger->d.cpu->gprs[r << 2],
			debugger->d.cpu->gprs[(r << 2) + 1],
			debugger->d.cpu->gprs[(r << 2) + 2],
			debugger->d.cpu->gprs[(r << 2) + 3]);
	}
	_printPSR(debugger->d.cpu->cpsr);
	int instructionLength;
	enum ExecutionMode mode = debugger->d.cpu->cpsr.t;
	if (mode == MODE_ARM) {
		instructionLength = WORD_SIZE_ARM;
	} else {
		instructionLength = WORD_SIZE_THUMB;
	}
	_printLine(debugger, debugger->d.cpu->gprs[ARM_PC] - instructionLength, mode);
}

static void _quit(struct CLIDebugger* debugger, struct DebugVector* dv) {
	UNUSED(dv);
	debugger->d.state = DEBUGGER_SHUTDOWN;
}

static void _readByte(struct CLIDebugger* debugger, struct DebugVector* dv) {
	if (!dv || dv->type != DV_INT_TYPE) {
		printf("%s\n", ERROR_MISSING_ARGS);
		return;
	}
	uint32_t address = dv->intValue;
	uint8_t value = debugger->d.cpu->memory.loadU8(debugger->d.cpu, address, 0);
	printf(" 0x%02X\n", value);
}

static void _readHalfword(struct CLIDebugger* debugger, struct DebugVector* dv) {
	if (!dv || dv->type != DV_INT_TYPE) {
		printf("%s\n", ERROR_MISSING_ARGS);
		return;
	}
	uint32_t address = dv->intValue;
	uint16_t value = debugger->d.cpu->memory.loadU16(debugger->d.cpu, address, 0);
	printf(" 0x%04X\n", value);
}

static void _readWord(struct CLIDebugger* debugger, struct DebugVector* dv) {
	if (!dv || dv->type != DV_INT_TYPE) {
		printf("%s\n", ERROR_MISSING_ARGS);
		return;
	}
	uint32_t address = dv->intValue;
	uint32_t value = debugger->d.cpu->memory.load32(debugger->d.cpu, address, 0);
	printf(" 0x%08X\n", value);
}

static void _setBreakpoint(struct CLIDebugger* debugger, struct DebugVector* dv) {
	if (!dv || dv->type != DV_INT_TYPE) {
		printf("%s\n", ERROR_MISSING_ARGS);
		return;
	}
	uint32_t address = dv->intValue;
	ARMDebuggerSetBreakpoint(&debugger->d, address);
}

static void _clearBreakpoint(struct CLIDebugger* debugger, struct DebugVector* dv) {
	if (!dv || dv->type != DV_INT_TYPE) {
		printf("%s\n", ERROR_MISSING_ARGS);
		return;
	}
	uint32_t address = dv->intValue;
	ARMDebuggerClearBreakpoint(&debugger->d, address);
}

static void _setWatchpoint(struct CLIDebugger* debugger, struct DebugVector* dv) {
	if (!dv || dv->type != DV_INT_TYPE) {
		printf("%s\n", ERROR_MISSING_ARGS);
		return;
	}
	uint32_t address = dv->intValue;
	ARMDebuggerSetWatchpoint(&debugger->d, address);
}

static void _breakIntoDefault(int signal) {
	UNUSED(signal);
	ARMDebuggerEnter(&_activeDebugger->d, DEBUGGER_ENTER_MANUAL);
}

static uint32_t _performOperation(enum Operation operation, uint32_t current, uint32_t next, struct DebugVector* dv) {
	switch (operation) {
	case OP_ASSIGN:
		current = next;
		break;
	case OP_ADD:
		current += next;
		break;
	case OP_SUBTRACT:
		current -= next;
		break;
	case OP_MULTIPLY:
		current *= next;
		break;
	case OP_DIVIDE:
		if (next != 0) {
			current /= next;
		} else {
			dv->type = DV_ERROR_TYPE;
			return 0;
		}
		break;
	}
	return current;
}

static uint32_t _evaluateParseTree(struct ParseTree* tree, struct DebugVector* dv) {
	switch (tree->token.type) {
	case TOKEN_UINT_TYPE:
		return tree->token.uintValue;
	case TOKEN_OPERATOR_TYPE:
		return _performOperation(tree->token.operatorValue, _evaluateParseTree(tree->lhs, dv), _evaluateParseTree(tree->rhs, dv), dv);
	case TOKEN_IDENTIFIER_TYPE:
	case TOKEN_ERROR_TYPE:
		dv->type = DV_ERROR_TYPE;
	}
	return 0;
}

static struct DebugVector* _DVParse(struct CLIDebugger* debugger, const char* string, size_t length) {
	if (!string || length < 1) {
		return 0;
	}

	struct DebugVector dvTemp = { .type = DV_INT_TYPE };

	struct LexVector lv = { .next = 0 };
	size_t adjusted = lexExpression(&lv, string, length);
	if (adjusted > length) {
		dvTemp.type = DV_ERROR_TYPE;
		lexFree(lv.next);
	}

	struct ParseTree tree;
	parseLexedExpression(&tree, &lv);
	if (tree.token.type == TOKEN_ERROR_TYPE) {
		dvTemp.type = DV_ERROR_TYPE;
	} else {
		dvTemp.intValue = _evaluateParseTree(&tree, &dvTemp);
	}

	parseFree(tree.lhs);
	parseFree(tree.rhs);

	length -= adjusted;
	string += adjusted;

	struct DebugVector* dv = malloc(sizeof(struct DebugVector));
	if (dvTemp.type == DV_ERROR_TYPE) {
		dv->type = DV_ERROR_TYPE;
		dv->next = 0;
	} else {
		*dv = dvTemp;
		if (string[0] == ' ') {
			dv->next = _DVParse(debugger, string + 1, length - 1);
			if (dv->next && dv->next->type == DV_ERROR_TYPE) {
				dv->type = DV_ERROR_TYPE;
			}
		}
	}
	return dv;
}

static void _DVFree(struct DebugVector* dv) {
	struct DebugVector* next;
	while (dv) {
		next = dv->next;
		free(dv);
		dv = next;
	}
}

static int _parse(struct CLIDebugger* debugger, const char* line, size_t count) {
	const char* firstSpace = strchr(line, ' ');
	size_t cmdLength;
	struct DebugVector* dv = 0;
	if (firstSpace) {
		cmdLength = firstSpace - line;
		dv = _DVParse(debugger, firstSpace + 1, count - cmdLength - 1);
		if (dv && dv->type == DV_ERROR_TYPE) {
			printf("Parse error\n");
			_DVFree(dv);
			return 0;
		}
	} else {
		cmdLength = count;
	}

	int i;
	const char* name;
	for (i = 0; (name = _debuggerCommands[i].name); ++i) {
		if (strlen(name) != cmdLength) {
			continue;
		}
		if (strncasecmp(name, line, cmdLength) == 0) {
			_debuggerCommands[i].command(debugger, dv);
			_DVFree(dv);
			return 1;
		}
	}
	_DVFree(dv);
	printf("Command not found\n");
	return 0;
}

static char* _prompt(EditLine* el) {
	UNUSED(el);
	return "> ";
}

static void _commandLine(struct ARMDebugger* debugger) {
	struct CLIDebugger* cliDebugger = (struct CLIDebugger*) debugger;
	const char* line;
	_printStatus(cliDebugger, 0);
	int count = 0;
	HistEvent ev;
	while (debugger->state == DEBUGGER_PAUSED) {
		line = el_gets(cliDebugger->elstate, &count);
		if (!line) {
			debugger->state = DEBUGGER_EXITING;
			return;
		}
		if (line[0] == '\n') {
			if (history(cliDebugger->histate, &ev, H_FIRST) >= 0) {
				_parse(cliDebugger, ev.str, strlen(ev.str) - 1);
			}
		} else {
			if (_parse(cliDebugger, line, count - 1)) {
				history(cliDebugger->histate, &ev, H_ENTER, line);
			}
		}
	}
}

static void _reportEntry(struct ARMDebugger* debugger, enum DebuggerEntryReason reason) {
	UNUSED(debugger);
	switch (reason) {
	case DEBUGGER_ENTER_MANUAL:
	case DEBUGGER_ENTER_ATTACHED:
		break;
	case DEBUGGER_ENTER_BREAKPOINT:
		printf("Hit breakpoint\n");
		break;
	case DEBUGGER_ENTER_WATCHPOINT:
		printf("Hit watchpoint\n");
		break;
	case DEBUGGER_ENTER_ILLEGAL_OP:
		printf("Hit illegal opcode\n");
		break;
	}
}

static unsigned char _tabComplete(EditLine* elstate, int ch) {
	UNUSED(ch);
	const LineInfo* li = el_line(elstate);
	const char* commandPtr;
	int cmd = 0, len = 0;
	const char* name = 0;
	for (commandPtr = li->buffer; commandPtr <= li->cursor; ++commandPtr, ++len) {
		for (; (name = _debuggerCommands[cmd].name); ++cmd) {
			int cmp = strncasecmp(name, li->buffer, len);
			if (cmp > 0) {
				return CC_ERROR;
			}
			if (cmp == 0) {
				break;
			}
		}
	}
	if (_debuggerCommands[cmd + 1].name && strncasecmp(_debuggerCommands[cmd + 1].name, li->buffer, len - 1) == 0) {
		return CC_ERROR;
	}
	name += len - 1;
	el_insertstr(elstate, name);
	el_insertstr(elstate, " ");
	return CC_REDISPLAY;
}

static void _cliDebuggerInit(struct ARMDebugger* debugger) {
	struct CLIDebugger* cliDebugger = (struct CLIDebugger*) debugger;
	// TODO: get argv[0]
	cliDebugger->elstate = el_init(BINARY_NAME, stdin, stdout, stderr);
	el_set(cliDebugger->elstate, EL_PROMPT, _prompt);
	el_set(cliDebugger->elstate, EL_EDITOR, "emacs");

	el_set(cliDebugger->elstate, EL_CLIENTDATA, cliDebugger);
	el_set(cliDebugger->elstate, EL_ADDFN, "tab-complete", "Tab completion", _tabComplete);
	el_set(cliDebugger->elstate, EL_BIND, "\t", "tab-complete", 0);
	cliDebugger->histate = history_init();
	HistEvent ev;
	history(cliDebugger->histate, &ev, H_SETSIZE, 200);
	el_set(cliDebugger->elstate, EL_HIST, history, cliDebugger->histate);
	_activeDebugger = cliDebugger;
	signal(SIGINT, _breakIntoDefault);
}

static void _cliDebuggerDeinit(struct ARMDebugger* debugger) {
	struct CLIDebugger* cliDebugger = (struct CLIDebugger*) debugger;
	history_end(cliDebugger->histate);
	el_end(cliDebugger->elstate);
}

void CLIDebuggerCreate(struct CLIDebugger* debugger) {
	ARMDebuggerCreate(&debugger->d);
	debugger->d.init = _cliDebuggerInit;
	debugger->d.deinit = _cliDebuggerDeinit;
	debugger->d.paused = _commandLine;
	debugger->d.entered = _reportEntry;
}
