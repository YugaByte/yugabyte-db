%{ /* -*- C++ -*- */
/*--------------------------------------------------------------------------------------------------
 * NOTE:
 * - All entities in this modules are copies of PostgreQL's code. We made some minor changes
 *   to avoid lint errors such as using '{' for if blocks.
 * - Do not use c++ commenting style (//) here because MAC Flex 2.6.2 has problem processing it.
 *   Example: Flex raise error for the following comment
 *            // processing a single quote (') in c++ comment within a rule action.
 * -------------------------------------------------------------------------------------------------
 */

/*--------------------------------------------------------------------------------------------------
 * Portions Copyright (c) YugaByte, Inc.
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * Lexical scanner for PostgreSQL
 *
 * The rules in this file must be kept in sync with psql's lexer!!!
 *
 * The rules are designed so that the scanner never has to backtrack,
 * in the sense that there is always a rule that can match the input
 * consumed so far (the rule action may internally throw back some input
 * with yyless(), however).  As explained in the flex manual, this makes
 * for a useful speed increase --- about a third faster than a plain -CF
 * lexer, in simple testing.  The extra complexity is mostly in the rules
 * for handling float numbers and continued string literals.  If you change
 * the lexical rules, verify that you haven't broken the no-backtrack
 * property by running flex with the "-b" option and checking that the
 * resulting "lex.backup" file says that no backing up is needed.  (As of
 * Postgres 9.2, this check is made automatically by the Makefile.)
 *--------------------------------------------------------------------------------------------------
 */

#include "yb/yql/cql/ql/parser/parser.h"
#include "yb/yql/cql/ql/parser/parse_context.h"
#include "yb/yql/cql/ql/parser/scanner.h"
#include "yb/yql/cql/ql/parser/scanner_util.h"
#include "yb/util/logging.h"

/* Not including unistd. */
#define YY_NO_UNISTD_H

using namespace std;
using namespace yb::ql;

/*
 * Flex 2.6.3 generates cc file which raises warning wile compilation by gcc 7.5.x
 * scanner_lex.l.cc:3318:28: error: the compiler can assume that the address of ‘file’ will never be NULL [-Werror=address]
 */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Waddress"
#endif

#undef YY_DECL
#define YY_DECL \
  GramProcessor::symbol_type LexProcessor::yylex(const ScanState& scan_state)

/* yyterminate is called to end yylex. */
#define yyterminate() return GramProcessor::make_END(cursor_)

/* YY_USER_ACTION is called to advance location after each time a pattern is matched. */
#define YY_USER_ACTION cursor_.columns(yyleng);

/* Lexxer "yyerror" is used only when the scanner failed to understand the input string when
 * translating it into tokens. When this serious error occurs, the compilation stops immediately.
 *
 * yyerror stops the scanning process, resets the scan state to INITIAL, ignores the input string
 * entirely, and return SCAN_ERROR. If the input string has more than one statements, all statements
 * are ignored if one of them failes to compile.
 */
#define yyerror(msg) \
do { \
  BEGIN(INITIAL); \
  return ScanError(msg); \
} while (false)

/* Each call to yylex must set yylloc to the location of the found token. When we parse a token
 * that requires multiple lexer rules to process, this should be done in the first such rule, else
 * yylloc will point into the middle of the token.
 */
#define SET_YYLLOC() (token_loc_ = cursor_)

/* Advance yylloc by the given number of bytes. */
#define ADVANCE_YYLLOC(delta) AdvanceCursor(delta)
%}

/*------------------------------------------------------------------------------------------------*/
/* Flex options. */
%option 8bit never-interactive
%option nodefault noinput nounput noyywrap batch warn
%option c++
%option yyclass="yb::ql::LexProcessor"
%option noyyalloc noyyrealloc noyyfree

/*------------------------------------------------------------------------------------------------*/
/* LEX definitions */

/*
 * OK, here is a short description of lex/flex rules behavior.
 * The longest pattern which matches an input string is always chosen.
 * For equal-length patterns, the first occurring in the rules list is chosen.
 * INITIAL is the starting state, to which all non-conditional rules apply.
 * Exclusive states change parsing rules while the state is active.  When in
 * an exclusive state, only those rules defined for that state apply.
 *
 * We use exclusive states for quoted strings, extended comments,
 * and to eliminate parsing troubles for numeric strings.
 * Exclusive states:
 *  <xb> bit string literal
 *  <xc> extended C-style comments
 *  <xd> delimited identifiers (double-quoted identifiers)
 *  <xh> hexadecimal numeric string
 *  <xq> standard quoted strings
 *  <xe> extended quoted strings (support backslash escape sequences)
 *  <xdolq> $foo$ quoted strings
 *  <xui> quoted identifier with Unicode escapes
 *  <xuiend> end of a quoted identifier with Unicode escapes, UESCAPE can follow
 *  <xus> quoted string with Unicode escapes
 *  <xusend> end of a quoted string with Unicode escapes, UESCAPE can follow
 *  <xeu> Unicode surrogate pair in extended quoted string
 *
 * Remember to add an <<EOF>> case whenever you add a new exclusive state!
 * The default one is probably not the right thing.
 */
%x xb
%x xc
%x xd
%x xh
%x xe
%x xq
%x xdolq
%x xui
%x xuiend
%x xus
%x xusend
%x xeu

/*
 * In order to make the world safe for Windows and Mac clients as well as
 * Unix ones, we accept either \n or \r as a newline.  A DOS-style \r\n
 * sequence will be seen as two successive newlines, but that doesn't cause
 * any problems.  Comments that start with -- and extend to the next
 * newline are treated as equivalent to a single whitespace character.
 *
 * NOTE a fine point: if there is no newline following --, we will absorb
 * everything to the end of the input as a comment.  This is correct.  Older
 * versions of Postgres failed to recognize -- as a comment if the input
 * did not end with a newline.
 *
 * XXX perhaps \f (formfeed) should be treated as a newline as well?
 *
 * XXX if you change the set of whitespace characters, fix scanner_isspace()
 * to agree, and see also the plpgsql lexer.
 */
horiz_space               [ \t\f]
newline                   [\n\r]
non_newline               [^\n\r]
space                     ({horiz_space}|{newline})

comment                   ("--"{non_newline}*)

whitespace                ({space}+|{comment})

/*
 * SQL requires at least one newline in the whitespace separating
 * string literals that are to be concatenated.  Silly, but who are we
 * to argue?  Note that {whitespace_with_newline} should not have * after
 * it, whereas {whitespace} should generally have a * after it...
 */
special_whitespace        ({space}+|{comment}{newline})
horiz_whitespace          ({horiz_space}|{comment})
whitespace_with_newline   ({horiz_whitespace}*{newline}{special_whitespace}*)

/*
 * To ensure that {quotecontinue} can be scanned without having to back up
 * if the full pattern isn't matched, we include trailing whitespace in
 * {quotestop}.  This matches all cases where {quotecontinue} fails to match,
 * except for {quote} followed by whitespace and just one "-" (not two,
 * which would start a {comment}).  To cover that we have {quotefail}.
 * The actions for {quotestop} and {quotefail} must throw back characters
 * beyond the quote (') proper.
 */
quote                     '
quotestop                 {quote}{whitespace}*
quotecontinue             {quote}{whitespace_with_newline}{quote}
quotefail                 {quote}{whitespace}*"-"

/* Bit string
 * It is tempting to scan the string for only those characters
 * which are allowed. However, this leads to silently swallowed
 * characters if illegal characters are included in the string.
 * For example, if xbinside is [01] then B'ABCD' is interpreted
 * as a zero-length string, and the ABCD' is lost!
 * Better to pass the string forward and let the input routines
 * validate the contents.
 */
xbstart                   [bB]{quote}
xbinside                  [^']*

/* Hexadecimal number */
xhstart                   [xX]{quote}
xhinside                  [^']*

/* National character */
xnstart                   [nN]{quote}

/* Quoted string that allows backslash escapes */
xestart                   [eE]{quote}
xeinside                  [^\\']+
xeescape                  [\\][^0-7]
xeoctesc                  [\\][0-7]{1,3}
xehexesc                  [\\]x[0-9A-Fa-f]{1,2}
xeunicode                 [\\](u[0-9A-Fa-f]{4}|U[0-9A-Fa-f]{8})
xeunicodefail             [\\](u[0-9A-Fa-f]{0,3}|U[0-9A-Fa-f]{0,7})

/* Extended quote
 * xqdouble implements embedded quote, ''''
 */
xqstart                   {quote}
xqdouble                  {quote}{quote}
xqinside                  [^']+

/* $foo$ style quotes ("dollar quoting")
 * The quoted string starts with $foo$ where "foo" is an optional string
 * in the form of an identifier, except that it may not contain "$",
 * and extends to the first occurrence of an identical string.
 * There is *no* processing of the quoted text.
 *
 * {dolqfailed} is an error rule to avoid scanner backup when {dolqdelim}
 * fails to match its trailing "$".
 */
dolq_start                [A-Za-z\200-\377_]
dolq_cont                 [A-Za-z\200-\377_0-9]
dolqdelim                 \$({dolq_start}{dolq_cont}*)?\$
dolqfailed                \${dolq_start}{dolq_cont}*
dolqinside                [^$]+

/* Double quote
 * Allows embedded spaces and other special characters into identifiers.
 */
dquote                    \"
xdstart                   {dquote}
xdstop                    {dquote}
xddouble                  {dquote}{dquote}
xdinside                  [^"]+

/* Unicode escapes */
uescape                   [uU][eE][sS][cC][aA][pP][eE]{whitespace}*{quote}[^']{quote}
/* error rule to avoid backup */
uescapefail               [uU][eE][sS][cC][aA][pP][eE]{whitespace}*"-"|[uU][eE][sS][cC][aA][pP][eE]{whitespace}*{quote}[^']|[uU][eE][sS][cC][aA][pP][eE]{whitespace}*{quote}|[uU][eE][sS][cC][aA][pP][eE]{whitespace}*|[uU][eE][sS][cC][aA][pP]|[uU][eE][sS][cC][aA]|[uU][eE][sS][cC]|[uU][eE][sS]|[uU][eE]|[uU]

/* Quoted identifier with Unicode escapes */
xuistart                  [uU]&{dquote}

/* Quoted string with Unicode escapes */
xusstart                  [uU]&{quote}

/* Optional UESCAPE after a quoted string or identifier with Unicode escapes. */
xustop1                   {uescapefail}?
xustop2                   {uescape}

/* error rule to avoid backup */
xufailed                  [uU]&

/* C-style comments
 *
 * The "extended comment" syntax closely resembles allowable operator syntax.
 * The tricky part here is to get lex to recognize a string starting with
 * slash-star as a comment, when interpreting it as an operator would produce
 * a longer match --- remember lex will prefer a longer match!  Also, if we
 * have something like plus-slash-star, lex will think this is a 3-character
 * operator whereas we want to see it as a + operator and a comment start.
 * The solution is two-fold:
 * 1. append {op_chars}* to xcstart so that it matches as much text as
 *    {operator} would. Then the tie-breaker (first matching rule of same
 *    length) ensures xcstart wins.  We put back the extra stuff with yyless()
 *    in case it contains a star-slash that should terminate the comment.
 * 2. In the operator rule, check for slash-star within the operator, and
 *    if found throw it back with yyless().  This handles the plus-slash-star
 *    problem.
 * Dash-dash comments have similar interactions with the operator rule.
 *
 * Add " to negate quote.
 */
xcstart                   \/\*{op_chars}*
xcstop                    \*+\/
xcinside                  [^*/]+

digit                     [0-9]
hexdigit                  [0-9A-Fa-f]
ident_start               [A-Za-z\200-\377_]
ident_cont                [A-Za-z\200-\377_0-9\$]

identifier                {ident_start}{ident_cont}*

/* Assorted special-case operators and operator-like tokens */
typecast                  "::"
dot_dot                   \.\.
colon_equals              ":="
equals_greater            "=>"
less_equals               "<="
greater_equals            ">="
less_greater              "<>"
not_equals                "!="
single_arrow              "->"
double_arrow              "->>"

/*
 * "self" is the set of chars that should be returned as single-character
 * tokens.  "op_chars" is the set of chars that can make up "Op" tokens,
 * which can be one or more characters long (but if a single-char token
 * appears in the "self" set, it is not to be returned as an Op).  Note
 * that the sets overlap, but each has some chars that are not in the other.
 *
 * If you change either set, adjust the character lists appearing in the
 * rule for "operator"!
 */
self                      [,()\[\].;\:\?\+\-\*\/\%\^\<\>\=]
op_chars                  [\~\!\@\#\^\&\|\`\?\+\-\*\/\%\=]
operator                  {op_chars}+

/* we no longer allow unary minus in numbers.
 * instead we pass it separately to parser. there it gets
 * coerced via doNegate() -- Leon aug 20 1999
 *
 * {decimalfail} is used because we would like "1..10" to lex as 1, dot_dot, 10.
 *
 * {realfail1} and {realfail2} are added to prevent the need for scanner
 * backup when the {real} rule fails to match completely.
 */
integer                   {digit}+
decimal                   (({digit}*\.{digit}+)|({digit}+\.{digit}*))
decimalfail               {digit}+\.\.
real                      ({integer}|{decimal})[Ee][-+]?{digit}+
realfail1                 ({integer}|{decimal})[Ee]
realfail2                 ({integer}|{decimal})[Ee][-+]
binary                    0[xX]{hexdigit}*
uuid                      {hexdigit}{8}-{hexdigit}{4}-{hexdigit}{4}-{hexdigit}{4}-{hexdigit}{12}

param                     \${integer}

other                     .

/*
 * Dollar quoted strings are totally opaque, and no escaping is done on them.
 * Other quoted strings must allow some special characters such as single-quote
 *  and newline.
 * Embedded single-quotes are implemented both in the SQL standard
 *  style of two adjacent single quotes "''" and in the Postgres/Java style
 *  of escaped-quote "\'".
 * Other embedded escaped characters are matched explicitly and the leading
 *  backslash is dropped from the string.
 * Note that xcstart must appear before operator, as explained above!
 *  Also whitespace (comment) must appear before operator.
 */

/*------------------------------------------------------------------------------------------------*/
/* FLEX rules. */
%%

%{
  /* FLEX initial code: The following code block is executed every time yylex is called.
   * Reset the current scanning locations each time yylex is called to match new pattern.
   */
  cursor_.step();
%}

{newline} {
  cursor_.lines();
}

{whitespace} {
  /* Increase line number and ignore this token. */
  CountNewlineInToken(yytext);
}

{xcstart} {
  /* Save the location where token starts. */
  SET_YYLLOC();
  xcdepth_ = 0;
  BEGIN(xc);
  /* Put back any characters past slash-star; see above. */
  yyless(2);
}

<xc>{xcstart} {
  xcdepth_++;
  /* Put back any characters past slash-star; see above. */
  yyless(2);
}

<xc>{xcstop} {
  if (xcdepth_ <= 0) {
    BEGIN(INITIAL);
  } else {
    xcdepth_--;
  }
}

<xc>{xcinside} {
  /* Ignore. */
}

<xc>{op_chars} {
  /* Ignore. */
}

<xc>\*+ {
  /* Ignore. */
}

<xc><<EOF>> {
  yyerror("unterminated /* comment");
}

{xbstart} {
  /* Binary bit type.
   * At some point we should simply pass the string
   * forward to the parser and label it there.
   * In the meantime, place a leading "b" on the string
   * to mark it for the input routine as a binary string.
   */
  SET_YYLLOC();
  BEGIN(xb);
  startlit();
  addlitchar('b');
}

<xb>{quotestop} |
<xb>{quotefail} {
  yyless(1);
  BEGIN(INITIAL);
  return GramProcessor::make_BCONST(ScanLiteral(), cursor_);
}

<xh>{xhinside} |
<xb>{xbinside} {
  addlit(yytext, yyleng);
}

<xh>{quotecontinue} |
<xb>{quotecontinue} {
  /* Increase line number and ignore this token. */
  CountNewlineInToken(yytext);
}

<xb><<EOF>> {
  yyerror("unterminated bit string literal");
}

{xhstart} {
  /* Hexadecimal bit type.
   * At some point we should simply pass the string
   * forward to the parser and label it there.
   * In the meantime, place a leading "x" on the string
   * to mark it for the input routine as a hex string.
   */
  SET_YYLLOC();
  BEGIN(xh);
  startlit();
  addlitchar('x');
}

<xh>{quotestop} |
<xh>{quotefail} {
  yyless(1);
  BEGIN(INITIAL);
  return GramProcessor::make_XCONST(ScanLiteral(), cursor_);
}

<xh><<EOF>> {
  yyerror("unterminated hexadecimal string literal");
}

{xnstart} {
  /* National character.
   * We will pass this along as a normal character string,
   * but preceded with an internally-generated "NCHAR".
   */
  SET_YYLLOC();
  yyless(1);        /* eat only 'n' this time */

  const ScanKeyword &keyword = ScanKeywordLookup("nchar");
  if (keyword.is_valid()) {
    return GramProcessor::make_NCHAR(keyword.name(), cursor_);
  } else {
    /* If NCHAR isn't a keyword, just return "n" */
    return GramProcessor::make_IDENT(MakeString("n"), cursor_);
  }
}

{xqstart} {
  warn_on_first_escape_ = true;
  saw_non_ascii_ = false;
  SET_YYLLOC();
  if (standard_conforming_strings_)
    BEGIN(xq);
  else
    BEGIN(xe);
  startlit();
}

{xestart} {
  warn_on_first_escape_ = false;
  saw_non_ascii_ = false;
  SET_YYLLOC();
  BEGIN(xe);
  startlit();
}

{xusstart} {
  SET_YYLLOC();
  if (!standard_conforming_strings_) {
    ScanError("Unsafe use of string constant with Unicode escapes. String constants "
              "with Unicode escapes cannot be used when standard_conforming_strings_ "
              "is off",
              ErrorCode::FEATURE_NOT_SUPPORTED);
  }
  BEGIN(xus);
  startlit();
}

<xq,xe>{quotestop} |
<xq,xe>{quotefail} {
  yyless(1);
  BEGIN(INITIAL);

  /* Check that data remains valid if it might have been made invalid by unescaping any chars. */
  if (saw_non_ascii_)
    pg_verify_mbstr_len(literalbuf_, literallen_, false);
  return GramProcessor::make_SCONST(ScanLiteral(), cursor_);
}

<xus>{quotestop} |
<xus>{quotefail} {
  /* throw back all but the quote */
  yyless(1);
  /* xusend state looks for possible UESCAPE */
  BEGIN(xusend);
}

<xusend>{whitespace} {
  /* Stay in xusend state over whitespace.
   * Increase line number and ignore this token.
   */
  CountNewlineInToken(yytext);
}

<xusend>{other} |
<xusend>{xustop1} |
<xusend><<EOF>> {
  /* Increase line number. */
  CountNewlineInToken(yytext);

  /* no UESCAPE after the quote, throw back everything */
  yyless(0);
  BEGIN(INITIAL);
  char *str = litbuf_udeescape('\\');
  return GramProcessor::make_SCONST(MakeString(str), cursor_);
}

<xusend>{xustop2} {
  /* Increase line number. */
  CountNewlineInToken(yytext);

  /* found UESCAPE after the end quote */
  BEGIN(INITIAL);
  if (!check_uescapechar(yytext[yyleng-2])) {
    SET_YYLLOC();
    ADVANCE_YYLLOC(yyleng-2);
    yyerror("invalid Unicode escape character");
  }
  char *str = litbuf_udeescape(yytext[yyleng-2]);
  return GramProcessor::make_SCONST(MakeString(str), cursor_);
}

<xq,xe,xus>{xqdouble} {
  addlitchar('\'');
}

<xq,xus>{xqinside} {
  addlit(yytext, yyleng);
}

<xe>{xeinside} {
  addlit(yytext, yyleng);
}

<xe>{xeunicode} {
  pg_wchar c = strtoul(yytext+2, NULL, 16);

  check_escape_warning();

  if (is_utf16_surrogate_first(c)) {
    utf16_first_part_ = c;
    BEGIN(xeu);
  } else if (is_utf16_surrogate_second(c)) {
    yyerror("invalid Unicode surrogate pair");
  } else {
    addunicode(c);
  }
}

<xeu>{xeunicode} {
  pg_wchar c = strtoul(yytext+2, NULL, 16);

  if (!is_utf16_surrogate_second(c)) {
    yyerror("invalid Unicode surrogate pair");
  }

  c = surrogate_pair_to_codepoint(utf16_first_part_, c);
  addunicode(c);
  BEGIN(xe);
}

<xeu>.        { yyerror("invalid Unicode surrogate pair"); }
<xeu>\n       { yyerror("invalid Unicode surrogate pair"); }
<xeu><<EOF>>  { yyerror("invalid Unicode surrogate pair"); }

<xe,xeu>{xeunicodefail} {
  ScanError("Invalid Unicode escape. Unicode escapes must be \\uXXXX or \\UXXXXXXXX",
             ErrorCode::INVALID_ESCAPE_SEQUENCE);
}

<xe>{xeescape} {
  if (yytext[1] == '\'') {
    if (backslash_quote_ == BackslashQuoteType::OFF ||
        (backslash_quote_ == BackslashQuoteType::SAFE_ENCODING)) {
      ScanError("Unsafe use of \\' in a string literal. Use '' to write quotes in "
                 "strings. \\' is insecure in client-only encodings",
                 ErrorCode::NONSTANDARD_USE_OF_ESCAPE_CHARACTER);
    }
  }
  check_string_escape_warning(yytext[1]);
  addlitchar(unescape_single_char(yytext[1]));
}

<xe>{xeoctesc} {
  unsigned char c = strtoul(yytext+1, NULL, 8);

  check_escape_warning();
  addlitchar(c);
  if (c == '\0' || is_utf_highbit_set(c)) {
    saw_non_ascii_ = true;
  }
}

<xe>{xehexesc} {
  unsigned char c = strtoul(yytext+2, NULL, 16);

  check_escape_warning();
  addlitchar(c);
  if (c == '\0' || is_utf_highbit_set(c)) {
    saw_non_ascii_ = true;
  }
}
<xq,xe,xus>{quotecontinue} {
  /* Increase line number and ignore this token. */
  CountNewlineInToken(yytext);
}

<xe>. {
  /* This is only needed for \ just before EOF */
  addlitchar(yytext[0]);
}

<xq,xe,xus><<EOF>> { yyerror("unterminated quoted string"); }

{dolqdelim} {
  SET_YYLLOC();
  dolqstart_ = MCStrdup(PTempMem(), yytext);
  BEGIN(xdolq);
  startlit();
}

{dolqfailed} {
  SET_YYLLOC();
  /* throw back all but the initial "$" */
  yyless(1);
  /* and treat it as {other} */
  return make_symbol(yytext[0], cursor_);
}

<xdolq>{dolqdelim} {
  if (strcmp(yytext, dolqstart_) == 0) {
    dolqstart_ = NULL;
    BEGIN(INITIAL);
    return GramProcessor::make_SCONST(ScanLiteral(), cursor_);
  } else {
    /* When we fail to match $...$ to dolqstart_, transfer
     * the $... part to the output, but put back the final
     * $ for rescanning.  Consider $delim$...$junk$delim$
     */
    addlit(yytext, yyleng-1);
    yyless(yyleng-1);
  }
}

<xdolq>{dolqinside} {
  addlit(yytext, yyleng);
}

<xdolq>{dolqfailed} {
  addlit(yytext, yyleng);
}

<xdolq>. {
  /* This is only needed for $ inside the quoted text */
  addlitchar(yytext[0]);
}

<xdolq><<EOF>>  { yyerror("unterminated dollar-quoted string"); }

{xdstart} {
  SET_YYLLOC();
  BEGIN(xd);
  startlit();
}

{xuistart} {
  SET_YYLLOC();
  BEGIN(xui);
  startlit();
}

<xd>{xdstop} {
  BEGIN(INITIAL);
  if (literallen_ == 0) {
    yyerror("zero-length delimited identifier");
  }
  const MCSharedPtr<MCString> ident = ScanLiteral();
  if (literallen_ >= NAMEDATALEN) {
    TruncateIdentifier(ident, true);
  }
  return GramProcessor::make_IDENT(ident, cursor_);
}

<xui>{dquote} {
  yyless(1);
  /* xuiend state looks for possible UESCAPE */
  BEGIN(xuiend);
}

<xuiend>{whitespace} {
  /* stay in xuiend state over whitespace.
   * Increase line number and ignore this token.
   */
  CountNewlineInToken(yytext);
}

<xuiend>{other} |
<xuiend>{xustop1} |
<xuiend><<EOF>> {
  /* Increase line number. */
  CountNewlineInToken(yytext);

  /* no UESCAPE after the quote, throw back everything */
  yyless(0);

  BEGIN(INITIAL);
  if (literallen_ == 0) {
    yyerror("zero-length delimited identifier");
  }

  const MCSharedPtr<MCString> ident = MakeString(litbuf_udeescape('\\'));
  TruncateIdentifier(ident, true);
  return GramProcessor::make_IDENT(ident, cursor_);
}

<xuiend>{xustop2} {
  /* Increase line number. */
  CountNewlineInToken(yytext);

  /* found UESCAPE after the end quote */
  BEGIN(INITIAL);
  if (literallen_ == 0) {
    yyerror("zero-length delimited identifier");
  }
  if (!check_uescapechar(yytext[yyleng-2])) {
    SET_YYLLOC();
    ADVANCE_YYLLOC(yyleng-2);
    yyerror("invalid Unicode escape character");
  }

  const MCSharedPtr<MCString> ident = MakeString(litbuf_udeescape(yytext[yyleng - 2]));
  TruncateIdentifier(ident, true);
  return GramProcessor::make_IDENT(ident, cursor_);
}

<xd,xui>{xddouble} {
  addlitchar('"');
}

<xd,xui>{xdinside} {
  addlit(yytext, yyleng);
}

<xd,xui><<EOF>> { yyerror("unterminated quoted identifier"); }

{xufailed} {
  SET_YYLLOC();

  /* Throw back all but the initial u/U. */
  yyless(1);

  /* Treat it as {identifier}. Convert to lower-case and create IDENT token. */
  const MCSharedPtr<MCString> ident = MakeIdentifier(yytext, yyleng, true);
  return GramProcessor::make_IDENT(ident, cursor_);
}

{typecast} {
  SET_YYLLOC();
  return GramProcessor::make_TYPECAST(cursor_);
}

{dot_dot} {
  SET_YYLLOC();
  return GramProcessor::make_DOT_DOT(cursor_);
}

{colon_equals} {
  SET_YYLLOC();
  return GramProcessor::make_COLON_EQUALS(cursor_);
}

{single_arrow} {
  SET_YYLLOC();
  return GramProcessor::make_SINGLE_ARROW(cursor_);
}

{double_arrow} {
  SET_YYLLOC();
  return GramProcessor::make_DOUBLE_ARROW(cursor_);
}

{equals_greater} {
  SET_YYLLOC();
  return GramProcessor::make_EQUALS_GREATER(cursor_);
}

{less_equals} {
  SET_YYLLOC();
  return GramProcessor::make_LESS_EQUALS(cursor_);
}

{greater_equals} {
  SET_YYLLOC();
  return GramProcessor::make_GREATER_EQUALS(cursor_);
}

{less_greater} {
  /* We accept both "<>" and "!=" as meaning NOT_EQUALS */
  SET_YYLLOC();
  return GramProcessor::make_NOT_EQUALS(cursor_);
}

{not_equals} {
  /* We accept both "<>" and "!=" as meaning NOT_EQUALS */
  SET_YYLLOC();
  return GramProcessor::make_NOT_EQUALS(cursor_);
}

{self} {
  SET_YYLLOC();
  return make_symbol(yytext[0], cursor_);
}

{operator} {
  /* Check for embedded slash-star or dash-dash; those
   * are comment starts, so operator must stop there.
   * Note that slash-star or dash-dash at the first
   * character will match a prior rule, not this one.
   */
  int   nchars = yyleng;
  char *slashstar = strstr(yytext, "/*");
  char *dashdash = strstr(yytext, "--");

  if (slashstar && dashdash) {
    /* if both appear, take the first one */
    if (slashstar > dashdash)
      slashstar = dashdash;
  } else if (!slashstar) {
    slashstar = dashdash;
  }
  if (slashstar) {
    nchars = slashstar - yytext;
  }

  /* For SQL compatibility, '+' and '-' cannot be the
   * last char of a multi-char operator unless the operator
   * contains chars that are not in SQL operators.
   * The idea is to lex '=-' as two operators, but not
   * to forbid operator names like '?-' that could not be
   * sequences of SQL operators. Similarly for '?' to allow
   * "... where c=?".
   */
  while (nchars > 1 && (yytext[nchars-1] == '+' ||
                        yytext[nchars-1] == '-' ||
                        yytext[nchars-1] == '?')) {
    int   ic;

    /* "!" is removed from the list below because
     * "... WHERE col!=?" should be returned as two separate
     * tokens ("!=" and "?").
     */
    for (ic = nchars-2; ic >= 0; ic--) {
      if (strchr("~@#^&|`?%", yytext[ic]))
        break;
    }
    if (ic >= 0)
      break; /* found a char that makes it OK */
    nchars--; /* else remove the +/-, and check again */
  }

  SET_YYLLOC();

  if (nchars < yyleng) {
    /* Strip the unwanted chars from the token */
    yyless(nchars);
    /* If what we have left is only one char, and it's
     * one of the characters matching "self", then
     * return it as a character token the same way
     * that the "self" rule would have.
     */
    if (nchars == 1 && strchr(",()[].;:+-*/%^<>=", yytext[0])) {
      return make_symbol(yytext[0], cursor_);
    }

    /* If what we have left are two chars, see if
     * it is one of the two-character operators.
     */
    if (nchars == 2) {
      if (strncmp(yytext, "::", 2) == 0) {
        return GramProcessor::make_TYPECAST(cursor_);
      } else if (strncmp(yytext, "..", 2) == 0) {
        return GramProcessor::make_DOT_DOT(cursor_);
      } else if (strncmp(yytext, ":=", 2) == 0) {
        return GramProcessor::make_COLON_EQUALS(cursor_);
      } else if (strncmp(yytext, "=>", 2) == 0) {
        return GramProcessor::make_EQUALS_GREATER(cursor_);
      } else if (strncmp(yytext, "<=", 2) == 0) {
        return GramProcessor::make_LESS_EQUALS(cursor_);
      } else if (strncmp(yytext, ">=", 2) == 0) {
        return GramProcessor::make_GREATER_EQUALS(cursor_);
      } else if (strncmp(yytext, "<>", 2) == 0 ||
                 strncmp(yytext, "!=", 2) == 0) {
        return GramProcessor::make_NOT_EQUALS(cursor_);
      }
    }
  }

  /* Complain if operator is too long.  Unlike the case
   * for identifiers, we make this an error not a notice-
   * and-truncate, because the odds are we are looking at
   * a syntactic mistake anyway.
   */
  if (nchars >= NAMEDATALEN)
    yyerror("operator too long");

  return GramProcessor::make_Op(MakeString(yytext), cursor_);
}

{param} {
  SET_YYLLOC();
  int64_t ival = atol(yytext + 1);
  return GramProcessor::make_PARAM(ival, cursor_);
}

{integer} {
  SET_YYLLOC();
  return GramProcessor::make_ICONST(MakeString(yytext), cursor_);
}

{decimal} {
  SET_YYLLOC();
  return GramProcessor::make_FCONST(MakeString(yytext), cursor_);
}

{decimalfail} {
  /* throw back the .., and treat as integer */
  yyless(yyleng-2);
  SET_YYLLOC();
  return GramProcessor::make_ICONST(MakeString(yytext), cursor_);
}

{binary} {
  SET_YYLLOC();
  // skipping the first two chars (i.e. the '0x')
  return GramProcessor::make_BCONST(MakeString(yytext + 2), cursor_);
}

{real} {
  SET_YYLLOC();
  return GramProcessor::make_FCONST(MakeString(yytext), cursor_);
}

{realfail1} {
  /* throw back the [Ee], and treat as {decimal}.  Note
   * that it is possible the input is actually {integer},
   * but since this case will almost certainly lead to a
   * syntax error anyway, we don't bother to distinguish.
   */
  yyless(yyleng-1);
  SET_YYLLOC();
  return GramProcessor::make_FCONST(MakeString(yytext), cursor_);
}

{realfail2} {
  /* throw back the [Ee][+-], and proceed as above */
  yyless(yyleng-2);
  SET_YYLLOC();
  return GramProcessor::make_FCONST(MakeString(yytext), cursor_);
}

{uuid} {
  SET_YYLLOC();
  /* Trim the dashes from the UUID string. */
  return GramProcessor::make_UCONST(MakeString(yytext), cursor_);
}

{identifier}  {
  SET_YYLLOC();

  /* Check against the keyword list. */
  const ScanKeyword &keyword = ScanKeywordLookup(yytext);
  if (keyword.is_valid()) {
    return make_symbol(keyword, cursor_);
  }

  /* Not a keyword.  Convert the identifier to lower case, and truncate if necessary. */
  const MCSharedPtr<MCString> ident = MakeIdentifier(yytext, yyleng, true);
  return GramProcessor::make_IDENT(ident, cursor_);
}

{other} {
  SET_YYLLOC();
  return make_symbol(yytext[0], cursor_);
}

<<EOF>> {
  SET_YYLLOC();
  yyterminate();
}

%%

/* FLEX C++ version doesn't allow us to change the specification of allocation routines, so we
 * cannot insert our own memory context here. As a result, to avoid memory fragmentation, we just
 * make sure that FLEX allocate its buffers by multiple of 4k.
 */
constexpr int kFlexBufferBlockSize = 4*1024;
static yy_size_t YYRoundUpFlexBuffer(yy_size_t size) {
  yy_size_t remainder = size % kFlexBufferBlockSize;
  if (remainder == 0) {
    return size;
  } else {
    return size + (kFlexBufferBlockSize - remainder);
  }
}

void *yyalloc(yy_size_t size) {
  /* Round up size to multiple of kMinFlexBufferSize. */
  size = YYRoundUpFlexBuffer(size);
  return malloc(size);
}

void *yyrealloc(void *ptr, yy_size_t size) {
  /* Round up size to multiple of kMinFlexBufferSize. */
  size = YYRoundUpFlexBuffer(size);
  return realloc(static_cast<char *>(ptr), size);
}

void yyfree(void *ptr) {
  free(static_cast<char *>(ptr));
}
