# Nothing undefined

A safe function has no behavior the language leaves undefined, but the one [the guarantees](/reference/guarantees/) state of a stale pointer: in the fast build, a pointer the rules did not follow may reach another object of its own type ((X6), (Y1) to (Y6)). This page is the list behind that sentence ([RFC 0051](/rfcs/0051-nothing-undefined/), (N1) to (N6)): every item of C's list of undefined behavior (C23, Annex J.2 of the working draft N3220, by its item numbers) and of C++'s (the working draft's annex of core undefined behavior, by its labels), each with how a safe function is kept from it and the test that shows it. `weavec-cc --weavec-explain undefined` prints it; `scripts/check-undefined.py` checks that every row has a disposition below and a test that exists.

| Disposition | Meaning |
| --- | --- |
| **rule** | A safe function that contains it does not compile; the row names the rule, or Clang's warning that is an error in a safe unit |
| **defined** | It has one result, which the row states |
| **stops** | The program ends with the report the row names, in both builds |
| **compared** | The link fails when the units disagree (`definition-mismatch`) |
| **stale** | A use of a pointer after its object: the rules, the guards, then (Y) in the fast build |
| **cannot** | It needs something a rule already rejects in a safe function; the row names the rule |
| **translation** | Clang rejects it, or gives it one meaning, when it translates the unit (and the linker when it links) |
| **library** | A precondition of the C or C++ library: its pointer arguments are guarded by the table's row (A2), its hardening checks what it checks (`library-assertion`), and the rest is assumed of the library |
| **assumed** | It is in code that is not safe, which the guarantees assume keeps them (T-A1) |

Rows of the same family say "as row N" or "as above". A row is one line of the source list in our own words, not the standard's.

## C

| J.2 | What | Disposition | How | Test |
| --- | --- | --- | --- | --- |
| 1 | A 'shall' outside a constraint is broken | translation | each of the rows below | `test/Safe/undefined-translation.c` |
| 2 | A source file ends without a new-line, or mid-token or comment | translation | Clang reads the file as if it ended in one; an open comment or token is an error | `test/Safe/undefined-translation.c` |
| 3 | Token pasting forms a universal character name | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 4 | A hosted program's main has no allowed form | translation | Clang rejects or warns; main is the program's entry either way | `test/Safe/undefined-translation.c` |
| 5 | Two threads race on an object | cannot | a safe function does not start a thread (not-modeled); a race between threads that code which is not safe starts is assumed (T-A1) | `test/Safe/cxx-expressions.cpp` |
| 6 | A character outside the source set appears outside literals | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 7 | A token holds an invalid multibyte character | translation | Clang rejects it or reads its bytes | `test/Safe/undefined-translation.c` |
| 8 | One identifier has internal and external linkage in a unit | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 9 | An object is used outside its lifetime | stale | ownership's rules; the guards (heap-use-after-free, stack-use-after-scope, stack-use-after-return); in the fast build (Y) | `test/Guards/use-after-scope.c` |
| 10 | A pointer to an object whose lifetime ended is used | stale | as row 9: the shadow, then reuse by type | `test/Guards/reference-load.c` |
| 11 | An automatic object is read before it is set | defined | every local of a safe function is zeroes until assigned (§2.3) | `test/Guards/ir-defined.c` |
| 12 | A non-value representation is read | rule | pointer-bytes and union-pointer keep bytes out of a bool; every other type's bytes are a value on the targets | `test/Safe/bool-bytes.cpp` |
| 13 | A non-value representation is written through a non-character lvalue | rule | as row 12 | `test/Safe/bool-bytes.cpp` |
| 14 | Two declarations of an object or function have incompatible types | compared | in one unit Clang rejects it; across units the link compares the records (definition-mismatch) | `test/Driver/definitions.cpp` |
| 15 | A composite VLA type's size expression is not evaluated | translation | Clang evaluates or rejects it | `test/Safe/undefined-translation.c` |
| 16 | A conversion to or from an integer type is out of range | defined | floating to integer saturates (§2); integer to floating rounds; integer to integer is defined by Clang | `test/Guards/ir-defined.c` |
| 17 | Demoting a floating value is out of range | defined | the targets' IEEE arithmetic gives an infinity | `test/Guards/ir-defined.c` |
| 18 | An lvalue designates no object | stops | a null base stops (null-dereference); a wild one by the guards and bounds | `test/Guards/null-base.c` |
| 19 | An incomplete non-array lvalue's value is used | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 20 | An uninitialized register-able automatic object is read | defined | zeroed (§2.3) | `test/Guards/ir-defined.c` |
| 21 | A register array decays to a pointer | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 22 | A void expression's value is used | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 23 | A pointer converts to an integer out of range | defined | Clang truncates the address; nothing is assumed of it | `test/Guards/ir-defined.c` |
| 24 | A pointer conversion produces a misaligned pointer | rule | pointer-cast rejects a conversion between object types but to bytes | `test/Safe/casts.c` |
| 25 | A function is called through an incompatible pointer type | rule | pointer-cast rejects a function pointer's conversion; declarations across units are compared | `test/Safe/casts.c` |
| 26 | An unmatched quote is lexed | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 27 | A reserved keyword is used for another purpose | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 28 | An identifier's universal character name is outside the allowed ranges | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 29 | An identifier starts with a universal character name for a digit | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 30 | Two identifiers differ only in nonsignificant characters | translation | every character of an identifier is significant to Clang | `test/Safe/undefined-translation.c` |
| 31 | __func__ is declared explicitly | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 32 | A string literal is modified | rule | string-write; a literal's bytes are read-only memory (memory-fault) | `test/Safe/typed-rules.c` |
| 33 | A header name holds a quote, backslash or comment opener | translation | Clang reads or rejects it | `test/Safe/undefined-translation.c` |
| 34 | Unsequenced side effects on one scalar | rule | -Wunsequenced is an error in a safe unit; what it does not see Clang evaluates in one order | `test/Safe/defined.cpp` |
| 35 | An exceptional condition in an expression (overflow) | defined | signed arithmetic wraps (§2) | `test/Guards/ir-defined.c` |
| 36 | An object is accessed through an lvalue of a type it may not be | rule | pointer-cast, union-pointer and pointer-bytes keep a safe function from reading memory as another type | `test/Safe/casts.c` |
| 37 | A function is defined with a type incompatible with the call's | compared | pointer-cast in a unit; the link compares declarations | `test/Driver/definitions.cpp` |
| 38 | A member of an atomic struct or union is accessed | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 39 | The unary * operand has an invalid value | stops | null-dereference, the guards and the bounds; a stale pointer as row 10 | `test/Guards/null-base.c` |
| 40 | A pointer is converted to a type other than integer or pointer | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 41 | The divisor of / or % is zero | stops | division-by-zero (§3.2) | `test/Guards/division.c` |
| 42 | The quotient of / or % is not representable | stops | division-overflow (§3.2) | `test/Guards/division.c` |
| 43 | Pointer arithmetic leaves its array | rule | index-single and the bounds: a safe function's arithmetic is on a pointer with a bound, and an access out of it stops | `test/Safe/index-single.c` |
| 44 | A one-past-the-end pointer is dereferenced | stops | the bounds (pointer-out-of-bounds) and the guards | `test/Guards/safe-bounds.c` |
| 45 | Pointers into different arrays are subtracted | defined | the difference of two addresses (GuardSafe drops `inbounds`); nothing is assumed of it | `test/Guards/ir-defined.c` |
| 46 | A subscript is out of range though memory is there | stops | index-out-of-bounds checks each index against its declared bound (B) | `test/Guards/array-bounds.c` |
| 47 | A pointer difference does not fit ptrdiff_t | defined | 64-bit targets: no object is that large; the subtraction wraps | `test/Guards/ir-defined.c` |
| 48 | A shift by a negative count or the width or more | defined | the count is taken modulo the width (§2) | `test/Guards/ir-defined.c` |
| 49 | A signed left shift overflows | defined | it wraps (§2) | `test/Guards/ir-defined.c` |
| 50 | Unrelated pointers are compared with < or > | defined | their addresses are compared | `test/Guards/ir-defined.c` |
| 51 | An object is assigned to an overlapping one | defined | a safe function's struct copy is a move (GuardDefine); library copies stop (overlapping-copy) | `test/Guards/ir-defined.c` |
| 52 | An integer constant expression has the wrong form | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 53 | An initializer's constant expression has the wrong form | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 54 | An arithmetic constant expression has the wrong form | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 55 | An address constant reads an object's value | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 56 | An object with no linkage has an incomplete type | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 57 | A block-scope function has a storage class other than extern | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 58 | A struct or union has no named member | translation | Clang accepts it as an extension with a size of zero | `test/Safe/undefined-translation.c` |
| 59 | A flexible array member is read where it has no elements | stops | the guards and the bounds (the object has no bytes there) | `test/Guards/heap-overflow.c` |
| 60 | An incomplete struct is not completed where it is needed | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 61 | A const object is changed | rule | pointer-cast rejects a cast that removes const; a const global is read-only memory (memory-fault) | `test/Safe/cxx-expressions.cpp` |
| 62 | A volatile object is accessed through a non-volatile lvalue | defined | the access is an ordinary one Clang emits; the object is where it is | `test/Guards/ir-defined.c` |
| 63 | A function type has qualifiers | translation | Clang rejects or ignores them | `test/Safe/undefined-translation.c` |
| 64 | Compatible qualified types differ in qualification | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 65 | Objects are accessed through restrict-qualified pointers that alias | rule | assumed: restrict is not accepted (§4.1) | `test/Safe/assumed.c` |
| 66 | A restrict pointer takes another restrict pointer's value | rule | assumed | `test/Safe/assumed.c` |
| 67 | An external inline function is not defined in its unit | translation | the link fails, or Clang emits the definition | `test/Safe/undefined-translation.c` |
| 68 | A _Noreturn function returns | rule | -Winvalid-noreturn is an error; a declaration is compared with the definition | `test/Safe/defined.cpp` |
| 69 | An object's declarations have different alignment specifiers | translation | Clang rejects it in a unit | `test/Safe/undefined-translation.c` |
| 70 | Declarations in different units have different alignments | compared | a global's record has its alignment | `test/Driver/definitions.cpp` |
| 71 | Compatible pointer types differ in qualification or pointee | translation | Clang rejects it in a unit; across units the link compares | `test/Safe/undefined-translation.c` |
| 72 | A VLA's size is not positive | stops | a negative size probes the stack (stack-exhaustion); zero gives no addressable byte (dynamic-stack-buffer-overflow) | `test/Guards/vla-overflow.c` |
| 73 | Compatible array types have different sizes | compared | in a unit Clang rejects it; a global's record has its bound | `test/Driver/definitions.cpp` |
| 74 | A [static n] parameter gets fewer elements | rule | assumed: 'static' in an array parameter is not accepted | `test/Safe/assumed.c` |
| 75 | void in a parameter list is qualified | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 76 | Compatible function types disagree | compared | in a unit Clang rejects it; across units the link compares | `test/Driver/definitions.cpp` |
| 77 | An inferred-type declaration has a pointer, array or function declarator | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 78 | An inferred-type declaration has no or several declarators | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 79 | An unnamed member's value is used | defined | zeroed with its object | `test/Guards/ir-defined.c` |
| 80 | A scalar's initializer has the wrong form | translation | Clang rejects it, or takes the first of a list and warns | `test/Safe/undefined-translation.c` |
| 81 | A struct's initializer has the wrong form | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 82 | An aggregate's initializer is not a braced list | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 83 | A function called as unsequenced or reproducible lacks the property | rule | assumed: an attribute that asserts is not accepted | `test/Safe/assumed.c` |
| 84 | An external identifier has no definition, or several | translation | the link fails | `test/Safe/undefined-translation.c` |
| 85 | A variadic function is defined without a prototype | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 86 | A function's closing brace is reached and its value used | stops | missing-return (§3.1) | `test/Guards/missing-return.c` |
| 87 | An internal object's tentative definition has an incomplete type | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 88 | A non-directive is executed | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 89 | defined is generated or misused in #if | translation | Clang defines or rejects it | `test/Safe/undefined-translation.c` |
| 90 | An #include does not match a header name form | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 91 | An #include's characters start with a non-letter | translation | Clang reads the name as given | `test/Safe/undefined-translation.c` |
| 92 | A macro argument holds what would be a directive | translation | Clang reads or rejects it | `test/Safe/undefined-translation.c` |
| 93 | # makes an invalid string literal | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 94 | ## makes an invalid token | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 95 | A #line directive is malformed or out of range | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 96 | A pragma documented as undefined is used | translation | Clang documents none | `test/Safe/undefined-translation.c` |
| 97 | A #pragma STDC is malformed | translation | Clang warns and ignores it | `test/Safe/undefined-translation.c` |
| 98 | A predefined macro or defined is defined or undefined | translation | Clang rejects or warns | `test/Safe/undefined-translation.c` |
| 99 | A library copy's operands overlap | stops | overlapping-copy (RFC 0035 §2.5) | `test/cases/detection/44_overlap_memcpy_remove/prog.c` |
| 100 | A file named as a standard header replaces it | translation | the include path decides; nothing is undefined in what is compiled | `test/Safe/undefined-translation.c` |
| 101 | A header is included inside a declaration | translation | Clang rejects what it makes | `test/Safe/undefined-translation.c` |
| 102 | A library name is used before its header | translation | Clang rejects an implicit declaration | `test/Safe/defined.c` |
| 103 | A standard header is included with a keyword defined as a macro | translation | Clang compiles the expansion; a broken one is rejected | `test/Safe/undefined-translation.c` |
| 104 | A library function is declared without external linkage | translation | Clang rejects or links its own | `test/Safe/undefined-translation.c` |
| 105 | A reserved identifier is declared | compared | Clang warns; a clash with the library's is a link error, and the runtime's names are its own | `test/Safe/undefined-translation.c` |
| 106 | A reserved macro is undefined | translation | Clang compiles what follows | `test/Safe/undefined-translation.c` |
| 107 | A library function gets an invalid value or a wrong variadic type | library | its pointer arguments are guarded by its row (A2); a format's are checked (-Wformat, the runtime's %s and %n); other values are assumed of the library | `test/Guards/checked-calls.c` |
| 108 | A library array parameter's pointer is invalid | stops | the row's extent is guarded | `test/Guards/checked-calls.c` |
| 109 | assert's macro is suppressed | translation | Clang compiles the call it names | `test/Safe/undefined-translation.c` |
| 110 | assert's argument is not scalar | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 111 | A floating pragma is placed where it may not be | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 112 | A character function's argument is out of range | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 113 | errno's macro is suppressed or errno defined | translation | Clang compiles it as written; a clash fails to link | `test/Safe/undefined-translation.c` |
| 114 | Floating status is used with FENV_ACCESS off | library | Clang's floating code is the target's; assumed of the library | `test/Guards/ir-defined.c` |
| 115 | A floating exception mask is invalid | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 116 | fesetexceptflag sets flags it was not given | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 117 | fesetenv gets an invalid environment | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 118 | An integer function's result is not representable | library | assumed of the library (A2): abs, the div family, strtol return the library's value | `test/Guards/checked-calls.c` |
| 119 | setlocale's string is modified | rule | string-write: a result the table does not call writable is read only | `test/Safe/typed-rules.c` |
| 120 | setlocale's string is used after another call | library | assumed of the library; its strings are not the runtime's heap | `test/Guards/checked-calls.c` |
| 121 | localeconv's structure is modified | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 122 | math_errhandling is suppressed or defined | translation | Clang compiles it as written | `test/Safe/undefined-translation.c` |
| 123 | A floating classification macro gets a non-floating argument | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 124 | setjmp is suppressed or defined | cannot | a safe function does not call setjmp (no-asm) | `test/Safe/forbidden.c` |
| 125 | setjmp is invoked where it may not be | cannot | as row 124 | `test/Safe/forbidden.c` |
| 126 | longjmp restores an environment that is gone | cannot | a safe function does not call longjmp (no-asm) | `test/Safe/forbidden.c` |
| 127 | A changed automatic object is read after longjmp | cannot | as row 126 | `test/Safe/forbidden.c` |
| 128 | A signal handler's pointer is invalid | rule | signal-handler: a safe function registers none | `test/Safe/signal-handler.cpp` |
| 129 | A handler returns from a computational exception | rule | as row 128 | `test/Safe/signal-handler.cpp` |
| 130 | A handler for SIGFPE, SIGILL or SIGSEGV returns | rule | as row 128; the runtime's own fault handler reports and ends the program | `test/Safe/signal-handler.cpp` |
| 131 | A handler for abort or raise calls raise | rule | as row 128 | `test/Safe/signal-handler.cpp` |
| 132 | An asynchronous handler reads a static object that is not lock-free atomic | rule | as row 128 | `test/Safe/signal-handler.cpp` |
| 133 | errno is read after a handler's SIG_ERR | rule | as row 128 | `test/Safe/signal-handler.cpp` |
| 134 | An asynchronous handler raises a signal | rule | as row 128 | `test/Safe/signal-handler.cpp` |
| 135 | signal is used in a multi-threaded program | rule | as row 128; SIG_IGN and SIG_DFL register no code | `test/Safe/signal-handler.cpp` |
| 136 | Variadic arguments are read without a proper va_list | rule | variadic: a safe function reads none | `test/Safe/calls.c` |
| 137 | va_arg is used on a va_list another function used | rule | variadic | `test/Safe/calls.c` |
| 138 | A va_ macro is suppressed or va_copy defined | rule | variadic | `test/Safe/calls.c` |
| 139 | va_start and va_end are unpaired | rule | variadic | `test/Safe/calls.c` |
| 140 | va_arg reads a missing or mistyped argument | rule | variadic | `test/Safe/calls.c` |
| 141 | va_arg's type is not an object type | rule | variadic | `test/Safe/calls.c` |
| 142 | An integer null pointer constant is read as a pointer by va_arg | rule | variadic | `test/Safe/calls.c` |
| 143 | A va_list is initialized twice without va_end | rule | variadic | `test/Safe/calls.c` |
| 144 | va_start gets unbalanced or unknown tokens | translation | Clang rejects them | `test/Safe/undefined-translation.c` |
| 145 | A generic function's macro is suppressed | translation | Clang compiles the function it names | `test/Safe/undefined-translation.c` |
| 146 | offsetof's type defines a new type | translation | Clang rejects or accepts it as an extension; offsetof is a constant | `test/Safe/undefined-translation.c` |
| 147 | unreachable() is reached | stops | unreachable (§3.1) | `test/Guards/ir-defined.c` |
| 148 | A nullptr_t object's bytes are changed, then read | defined | Clang reads a nullptr_t as null whatever its bytes | `test/Guards/ir-defined.c` |
| 149 | offsetof's member is invalid or a bit-field | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 150 | An integer-constant macro's argument is invalid | translation | Clang rejects or evaluates it | `test/Safe/undefined-translation.c` |
| 151 | Byte and wide I/O are mixed on a stream | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 152 | A wide stream is used past its last character | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 153 | A FILE is used after it was closed | library | assumed of the library (A2): a FILE is the library's memory, not the runtime's heap | `test/Guards/checked-calls.c` |
| 154 | fflush is applied to an input stream | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 155 | fopen's mode is not one of the forms | library | assumed of the library (A2); the mode string is guarded | `test/Guards/checked-calls.c` |
| 156 | Output and input alternate on a stream without positioning | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 157 | setvbuf's array is used | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 158 | A format has too few or mistyped arguments | rule | -Wformat is an error for a literal format; the runtime checks every %s and %n argument | `test/Safe/defined.cpp` |
| 159 | A format is not a valid multibyte sequence | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 160 | A precision appears with a conversion it does not fit | rule | -Wformat | `test/Safe/defined.cpp` |
| 161 | A * width or precision has no argument | rule | -Wformat | `test/Safe/defined.cpp` |
| 162 | # or 0 appears with a conversion it does not fit | rule | -Wformat | `test/Safe/defined.cpp` |
| 163 | A length modifier appears with a conversion it does not fit | rule | -Wformat | `test/Safe/defined.cpp` |
| 164 | %s's argument has no terminator | stops | unterminated-string: the runtime checks %s arguments | `test/Guards/checked-calls.c` |
| 165 | %n has flags, a width or a precision | rule | -Wformat | `test/Safe/defined.cpp` |
| 166 | % is followed by more than % | rule | -Wformat | `test/Safe/defined.cpp` |
| 167 | A conversion specification is invalid | rule | -Wformat | `test/Safe/defined.cpp` |
| 168 | A formatted output's count exceeds INT_MAX | library | the library returns -1 with EOVERFLOW (both targets' libraries); assumed (A2) | `test/Guards/checked-calls.c` |
| 169 | A formatted input assigns more than INT_MAX items | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 170 | A scanned value does not fit its object | library | the number parsers are the runtime's wrappers (RFC 0036 §6), which saturate | `test/Guards/scanf.c` |
| 171 | A %c, %s or %[ target is too small | stops | the scanned arguments are guarded after the call (RFC 0038 §2) | `test/Guards/scanf.c` |
| 172 | A wide %c, %s or %[ input is invalid | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 173 | A scanned %p was not printed by this execution | rule | int-to-pointer: a pointer is not made from a value in a safe function; %p's target is guarded | `test/Safe/casts.c` |
| 174 | A v*printf function gets an uninitialized va_list | rule | variadic | `test/Safe/calls.c` |
| 175 | fgets's array is used after a read error | defined | its bytes are the zeroed or earlier ones | `test/Guards/ir-defined.c` |
| 176 | fgets is given n of zero or less | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 177 | A binary stream's position is used after ungetc at zero | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 178 | A position is used after fread or fwrite failed | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 179 | A partial element fread read is used | rule | pointer-bytes: bytes go only into objects whose every byte pattern is a value | `test/Safe/bool-bytes.cpp` |
| 180 | fseek on a text stream uses an offset ftell did not give | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 181 | fsetpos sets a position fgetpos did not give | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 182 | A zero-size allocation is accessed | stops | the block has no addressable byte (heap-buffer-overflow) | `test/Guards/heap-overflow.c` |
| 183 | Released memory is used | stale | heap-use-after-free; in the fast build (Y) | `test/Guards/reference-load.c` |
| 184 | free or realloc gets a pointer the allocator did not give, or twice | stops | invalid-free, double-free: the runtime validates every release | `test/Guards/report-release.c` |
| 185 | malloc's uninitialized object is read | defined | the runtime's blocks are zeroes, and each allocation call says so to the optimizer (§2.3) | `test/Guards/fresh-blocks.c` |
| 186 | realloc's new bytes are read | defined | the runtime's blocks are zeroes | `test/Guards/ir-defined.c` |
| 187 | exit or quick_exit is called twice | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 188 | longjmp leaves an atexit function | cannot | a safe function does not call longjmp (no-asm) | `test/Safe/forbidden.c` |
| 189 | getenv's or strerror's string is modified | rule | string-write: their results are read only | `test/Safe/typed-rules.c` |
| 190 | A signal is raised during quick_exit | rule | signal-handler | `test/Safe/signal-handler.cpp` |
| 191 | system runs a command documented as undefined | library | the command is another program | `test/Guards/checked-calls.c` |
| 192 | A search or sort gets an invalid pointer | stops | the row guards the array (qsort, bsearch) | `test/Guards/checked-calls.c` |
| 193 | A sort's comparison changes the array or is inconsistent | library | assumed of the library (A2): the library reads inside the array it was given | `test/Guards/checked-calls.c` |
| 194 | bsearch's array is not sorted | library | the library's search reads inside the array; the result is one of its elements or null | `test/Guards/checked-calls.c` |
| 195 | A conversion state is used after LC_CTYPE changed | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 196 | A string function accesses past its object | stops | the row's extent is guarded, a string's terminator found within its object (unterminated-string) | `test/Guards/library-scan.c` |
| 197 | A string function gets an invalid pointer, even with length zero | stops | the row guards the pointer (null allowed only where the row says) | `test/Guards/checked-calls.c` |
| 198 | strxfrm's or strftime's array is used after a short length | defined | its bytes are what the call wrote and zeroes | `test/Guards/ir-defined.c` |
| 199 | strtok is called from several threads | cannot | a safe function does not start a thread; in safe C++ thread-share rejects a function with state of its own | `test/Safe/cxx-thread-share.cpp` |
| 200 | strtok's first call gets null | library | assumed of the library (A2): both libraries return null | `test/Guards/checked-calls.c` |
| 201 | strerror's string is used after another call | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 202 | A type-generic macro's argument type is incompatible | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 203 | A type-generic macro mixes standard and decimal floating types | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 204 | A type-generic macro has no function for its types | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 205 | A complex argument reaches a macro with no complex function | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 206 | A decimal floating argument reaches a complex macro | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 207 | A standard floating argument reaches a decimal macro | translation | Clang rejects it | `test/Safe/undefined-translation.c` |
| 208 | A thread locks a non-recursive mutex it holds | cannot | Safe C starts no thread; in safe C++ lock-held follows a class's one mutex; deadlock is not caught (it ends nothing) | `test/Safe/cxx-lock-held.cpp` |
| 209 | mtx_timedlock gets a mutex without timeout | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 210 | mtx_unlock gets a mutex the thread does not hold | cannot | as row 208: a guard object unlocks what it locked | `test/Safe/cxx-lock-held.cpp` |
| 211 | A thread is joined or detached twice | cannot | a safe function does not start a thread in C; in C++ a thread object joins once | `test/Safe/cxx-thread-member.cpp` |
| 212 | tss_create is called from a destructor | cannot | Safe C starts no thread (not-modeled) | `test/Safe/cxx-expressions.cpp` |
| 213 | A tss key is used after destructors began | cannot | as row 212 | `test/Safe/cxx-expressions.cpp` |
| 214 | A time function's result is used after its thread exited | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 215 | asctime gets a time out of range | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 216 | fwprintf's %s argument is an invalid multibyte string | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 217 | wcstok's state is not the previous call's | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 218 | An mbstate_t is misused | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 219 | A wide character function's argument is out of range | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 220 | iswctype is called under another LC_CTYPE | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |
| 221 | towctrans is called under another LC_CTYPE | library | assumed of the library (A2) | `test/Guards/checked-calls.c` |

## C++

| Entry | What | Disposition | How | Test |
| --- | --- | --- | --- | --- |
| `intro.object.implicit.create` | Implicit object creation can give no defined behavior | cannot | raw-owner: safe C++ makes objects with constructors, owners and containers, not over raw storage | `test/Safe/cxx-expressions.cpp` |
| `intro.object.implicit.pointer` | A pointer into implicitly created storage gives no defined behavior | cannot | as above | `test/Safe/cxx-expressions.cpp` |
| `basic.align.object.alignment` | An object is made in storage too little aligned for it | cannot | raw-owner rejects placement new; pointer-cast rejects a conversion to another object type | `test/Safe/cxx-expressions.cpp` |
| `lifetime.outside.pointer.delete` | delete through a pointer to an object whose lifetime ended | cannot | raw-owner rejects delete | `test/Safe/cxx-expressions.cpp` |
| `lifetime.outside.pointer.member` | A member is used through a pointer to an ended object | stale | stale-borrow, escape; a raw pointer member is a reference, guarded or in the fast build (Y) | `test/Safe/cxx-borrows.cpp` |
| `lifetime.outside.pointer.virtual` | A pointer to an ended object converts to a virtual base | stale | as above | `test/Safe/cxx-borrows.cpp` |
| `lifetime.outside.pointer.dynamic.cast` | dynamic_cast of a pointer to an ended object | stale | as above | `test/Safe/cxx-borrows.cpp` |
| `lifetime.outside.glvalue.access` | An ended object is accessed through a glvalue | stale | stale-borrow; the guards; in the fast build (Y) | `test/Safe/cxx-borrows.cpp` |
| `lifetime.outside.glvalue.member` | A member function is called on an ended object | stale | as above | `test/Safe/cxx-borrows.cpp` |
| `lifetime.outside.glvalue.virtual` | A reference to an ended object binds to a virtual base | stale | as above | `test/Safe/cxx-borrows.cpp` |
| `lifetime.outside.glvalue.dynamic.cast` | dynamic_cast or typeid of an ended object | stale | as above | `test/Safe/cxx-borrows.cpp` |
| `original.type.implicit.destructor` | An implicit destructor runs over storage of another type | cannot | raw-owner rejects placement new and a destructor called by hand | `test/Safe/cxx-expressions.cpp` |
| `creating.within.const.complete.obj` | An object is made over a const complete object | cannot | raw-owner rejects placement new | `test/Safe/cxx-expressions.cpp` |
| `basic.indet.value` | An indeterminate value is evaluated | defined | every local is zeroes until assigned (§2.3); the runtime's blocks are zeroes, and each allocation call says so to the optimizer | `test/Guards/fresh-blocks.cpp` |
| `basic.stc.alloc.dealloc.constraint` | An allocation function breaks its contract | assumed | a program's own operator new is code that is not safe (raw-owner keeps safe code from writing one) | `test/Safe/cxx-expressions.cpp` |
| `basic.stc.alloc.zero.dereference` | The block of a zero-size allocation is dereferenced | stops | the block has no addressable byte (heap-buffer-overflow) | `test/Guards/heap-overflow.c` |
| `basic.compound.invalid.pointer` | A released pointer is dereferenced or released again | stale | heap-use-after-free, double-free; in the fast build (Y) | `test/Guards/report-release.c` |
| `intro.execution.unsequenced.modification` | Unsequenced side effects conflict | rule | -Wunsequenced is an error in a safe unit; what it does not see Clang evaluates in one order | `test/Safe/defined.cpp` |
| `intro.races.data` | A data race | rule | thread-send, thread-share, thread-lent, lock-held (RFC 0046) | `test/Safe/cxx-thread-share.cpp` |
| `intro.progress.stops` | A thread stops making progress | defined | a loop may run forever: nothing is assumed of progress (§2) | `test/Guards/ir-defined.cpp` |
| `basic.start.main.exit.during.destruction` | std::exit is called during static destruction | library | assumed of the library (Y-A4) | `test/Guards/library-assertion.cpp` |
| `basic.start.term.use.after.destruction` | A destroyed static object is used during termination | stale | the guards; in the fast build (Y) | `test/Safe/cxx-borrows.cpp` |
| `expr.expr.eval` | A result is not mathematically defined or not representable | defined | signed arithmetic wraps; division stops (§2, §3.2) | `test/Guards/ir-defined.cpp` |
| `expr.basic.lvalue.strict.aliasing.violation` | An object is read through a glvalue of a dissimilar type | rule | pointer-cast rejects reinterpret_cast and a conversion between object types | `test/Safe/cxx-casts.cpp` |
| `expr.basic.lvalue.union.initialization` | A union is copied from what is not a live union | stale | a union holds no owner or pointer (union-pointer); its source is a borrow | `test/Safe/cxx-expressions.cpp` |
| `expr.type.reference.lifetime` | A reference is evaluated where its pointer would be invalid | stale | stale-borrow, escape | `test/Safe/cxx-borrows.cpp` |
| `conv.lval.valid.representation` | A value's representation is invalid for its type | rule | pointer-bytes and union-pointer keep bytes out of a bool; Clang assumes nothing of an enum's value | `test/Safe/bool-bytes.cpp` |
| `conv.double.out.of.range` | A floating conversion is out of range | defined | the targets' IEEE arithmetic gives an infinity | `test/Guards/ir-defined.cpp` |
| `conv.fpint.float.not.represented` | A floating value does not fit its integer type | defined | the conversion saturates, NaN to 0 (§2) | `test/Guards/ir-defined.cpp` |
| `conv.fpint.int.not.represented` | An integer does not fit its floating type | defined | IEEE rounding, or an infinity | `test/Guards/ir-defined.cpp` |
| `conv.ptr.virtual.base` | A pointer converts to a virtual base without an object | stale | the pointer is a borrow or a reference (rules, guards) | `test/Safe/cxx-borrows.cpp` |
| `conv.member.missing.member` | A member pointer converts to a class without the member | rule | pointer-cast rejects a member pointer's conversion to a base | `test/Safe/cxx-casts.cpp` |
| `expr.call.different.type` | A function is called through a type it does not have | rule | pointer-cast; the link compares declarations (definition-mismatch) | `test/Driver/definitions.cpp` |
| `expr.ref.member.not.similar` | A member is accessed on an object of a dissimilar type | rule | pointer-cast | `test/Safe/cxx-casts.cpp` |
| `expr.dynamic.cast.pointer.lifetime` | dynamic_cast of a pointer to a wrong or ended object | stale | as lifetime.outside | `test/Safe/cxx-borrows.cpp` |
| `expr.dynamic.cast.glvalue.lifetime` | dynamic_cast of a reference to a wrong or ended object | stale | as lifetime.outside | `test/Safe/cxx-borrows.cpp` |
| `expr.static.cast.base.class` | A base reference is static_cast to a type it is not in | rule | pointer-cast: a base is converted to a derived class without a check | `test/Safe/cxx-casts.cpp` |
| `expr.static.cast.enum.outside.range` | An integer outside an unfixed enum's range is cast to it | defined | Clang assumes nothing of an enum's value (-fstrict-enums is refused) | `test/Driver/safe-flags.cpp` |
| `expr.static.cast.fp.outside.range` | A floating value is cast outside its range | defined | the conversion saturates (§2) | `test/Guards/ir-defined.cpp` |
| `expr.static.cast.downcast.wrong.derived.type` | A downcast names an object that is not there | rule | pointer-cast | `test/Safe/cxx-casts.cpp` |
| `expr.static.cast.does.not.contain.original.member` | A member pointer is cast to a class without the member | rule | pointer-cast | `test/Safe/cxx-casts.cpp` |
| `expr.unary.dereference` | A pointer to no object is dereferenced | stops | null-dereference, the guards, the bounds; a stale one as lifetime.outside | `test/Guards/null-base.c` |
| `expr.new.non.allocating.null` | Placement new returns null | cannot | raw-owner rejects placement new | `test/Safe/cxx-expressions.cpp` |
| `expr.delete.mismatch` | delete[] of a single object | cannot | raw-owner rejects delete; alloc-release-mismatch stops it elsewhere | `test/Safe/cxx-expressions.cpp` |
| `expr.delete.array.mismatch` | delete of an array | cannot | as above | `test/Safe/cxx-expressions.cpp` |
| `expr.delete.dynamic.type.differ` | delete through a base without a virtual destructor | rule | pointer-cast: an owner of a base is not made from an owner of a derived object when the base's destructor is not virtual | `test/Safe/cxx-casts.cpp` |
| `expr.delete.dynamic.array.dynamic.type.differ` | delete[] where elements' types differ | cannot | raw-owner rejects delete | `test/Safe/cxx-expressions.cpp` |
| `expr.mptr.oper.not.contain.member` | A member pointer is applied to an object without the member | rule | pointer-cast rejects the conversions that make one | `test/Safe/cxx-casts.cpp` |
| `expr.mptr.oper.member.func.null` | A null member function pointer is called | stops | the call reaches the lowest page (null-dereference or memory-fault) | `test/Guards/memory-fault.c` |
| `expr.mul.div.by.zero` | Division by zero | stops | division-by-zero (§3.2) | `test/Guards/division.c` |
| `expr.mul.representable.type.result` | A quotient is not representable | stops | division-overflow (§3.2) | `test/Guards/division.c` |
| `expr.add.out.of.bounds` | A pointer is formed outside its array | rule | index-single, call-args; the bounds stop an access outside | `test/Safe/cxx-bounds.cpp` |
| `expr.add.sub.diff.pointers` | Pointers into different arrays are subtracted | defined | the difference of two addresses; nothing is assumed of it | `test/Guards/ir-defined.cpp` |
| `expr.add.not.similar` | Arithmetic on a pointer to a type the array's is not | rule | pointer-cast | `test/Safe/cxx-casts.cpp` |
| `expr.shift.neg.and.width` | A shift by a negative count or the width or more | defined | the count is taken modulo the width (§2) | `test/Guards/ir-defined.cpp` |
| `expr.assign.overlap` | An assignment's source overlaps its destination | defined | a safe function's copy of an object is a move (GuardDefine) | `test/Guards/ir-defined.c` |
| `stmt.return.flow.off` | A value-returning function flows off its end | stops | missing-return (§3.1) | `test/Guards/missing-return.cpp` |
| `stmt.return.coroutine.flow.off` | A coroutine flows off its end | cannot | not-modeled rejects a coroutine | `test/Safe/cxx-expressions.cpp` |
| `stmt.dcl.local.static.init.recursive` | A static local's initialization re-enters itself | stops | the C++ runtime's guard ends the program | `test/Guards/report-cxx.cpp` |
| `dcl.type.cv.modify.const.obj` | A const object is changed | rule | pointer-cast rejects a cast that removes const; const globals are read-only memory | `test/Safe/cxx-casts.cpp` |
| `dcl.type.cv.access.volatile` | A volatile object is accessed as non-volatile | defined | an ordinary access Clang emits | `test/Guards/ir-defined.cpp` |
| `dcl.ref.incompatible.function` | A function reference binds to an incompatible function | rule | pointer-cast | `test/Safe/cxx-casts.cpp` |
| `dcl.ref.incompatible.type` | An object is accessed through a reference that may not | rule | pointer-cast | `test/Safe/cxx-casts.cpp` |
| `dcl.ref.uninitialized.reference` | A reference is evaluated before it is bound | stops | what holds it is zeroes: the reference is null (null-dereference) | `test/Guards/null-base.c` |
| `dcl.fct.def.coroutine.resume.not.suspended` | A coroutine that is not suspended is resumed | cannot | not-modeled rejects a coroutine | `test/Safe/cxx-expressions.cpp` |
| `dcl.fct.def.coroutine.destroy.not.suspended` | A coroutine that is not suspended is destroyed | cannot | as above | `test/Safe/cxx-expressions.cpp` |
| `dcl.attr.assume.false` | An [[assume]] expression is false | rule | assumed: [[assume]] is not accepted (§4.1) | `test/Safe/assumed.cpp` |
| `dcl.attr.noreturn.eventually.returns` | A [[noreturn]] function returns | rule | -Winvalid-noreturn is an error; declarations are compared | `test/Safe/defined.cpp` |
| `class.dtor.no.longer.exists` | A destructor runs on an ended object | cannot | raw-owner rejects a destructor called by hand | `test/Safe/cxx-expressions.cpp` |
| `class.abstract.pure.virtual` | A pure virtual function is called in a constructor or destructor | stops | the C++ runtime's handler ends the program | `test/Guards/report-cxx.cpp` |
| `class.base.init.mem.fun` | A member function is called before the bases are initialized | defined | the object's bytes are zeroes before its constructor runs (zeroed locals, the runtime's zeroed blocks) | `test/Guards/ir-defined.cpp` |
| `class.cdtor.before.ctor` | A member is named before a non-trivial constructor starts | defined | as above | `test/Guards/ir-defined.cpp` |
| `class.cdtor.after.dtor` | A member is named after a non-trivial destructor ends | stale | the guards; in the fast build (Y) | `test/Safe/cxx-borrows.cpp` |
| `class.cdtor.convert.pointer` | A pointer converts to a base outside construction | defined | the base's address is computed from the object's; the object's bytes are as row class.base.init.mem.fun | `test/Guards/ir-defined.cpp` |
| `class.cdtor.form.pointer` | A member's pointer is formed outside construction | defined | as above | `test/Guards/ir-defined.cpp` |
| `class.cdtor.virtual.not.x` | A qualified virtual call in a constructor names a more derived class | translation | Clang rejects a call to a member of a class the object is not yet | `test/Safe/undefined-translation.cpp` |
| `class.cdtor.typeid` | typeid of an object under construction through an unrelated type | rule | pointer-cast keeps a safe function from naming an object through an unrelated type | `test/Safe/cxx-casts.cpp` |
| `class.cdtor.dynamic.cast` | dynamic_cast of an object under construction through an unrelated type | rule | as above | `test/Safe/cxx-casts.cpp` |
| `temp.inst.inf.recursion` | Template instantiation recurses without end | translation | Clang stops at its depth limit with an error | `test/Safe/undefined-translation.cpp` |
| `except.handle.handler.ctor.dtor` | A constructor's try-block handler names a member | stale | the members are destroyed: the guards; in the fast build (Y) | `test/Safe/cxx-borrows.cpp` |

## Beyond the lists

Clang's extensions are not on either list, and one of them is not yet disposed of ([RFC 0051](/rfcs/0051-nothing-undefined/), amendment 17): a division or remainder of vector types (`__attribute__((vector_size(n)))`, `ext_vector_type`) whose divisor has a zero lane, or a signed one with the least value divided by -1 in a lane, is not checked in a safe function. A safe function that divides vectors should test the divisor's lanes first.
