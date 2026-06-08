/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

const BRACE1 = [['{', '}']];
const BRACE2 = [['{{', '}}'], ['⦃', '⦄']];
const PAREN = [['(', ')']];

const integer = /\-?(0x[0-9a-fA-F]+|[0-9]+)/;

module.exports = grammar({
  name: 'agda',

  word: $ => $.id,

  extras: $ => [
    $.comment,
    $.block_comment,
    $.pragma,
    /\s|\\n/,
  ],

  externals: $ => [
    $._newline,
    $._indent,
    $._dedent,
    $._lambda_newline,
    $.block_comment,
    $._open_idiom, // `(|` — emitted only when followed by whitespace (Agda Issue #2186)
    $._empty_idiom, // `(|)`
    // `as` starting a rename clause — emitted only when the module name after it
    // is NOT itself followed by another `as`, so `open import M as as as` resolves
    // (1st `as` = module arg, 2nd = the rename keyword, 3rd = the alias).
    $._as_rename,
    // Marker externals — never emitted. Each appears in a rule as
    // `choice($._indent, $._X_indent)`, so valid_symbols[_X_indent] is true exactly
    // at that block's INDENT push, letting the scanner tag the layout-stack entry.
    // The tag is what later lets it tell a let / do / λ-where block apart from a
    // plain indented block when deciding how (and in what order) to close it.
    $._let_indent,
    $._do_indent,
    $._lam_where_indent,
    // Line comment. Routed through the scanner only so it can handle comment bodies
    // containing NUL (0x00), which the generated DFA's /[^\n]*/ stops at; ordinary
    // comments fall through to the built-in `comment` rule.
    $.comment,
  ],

  // After `λ` followed by some bindings, the parser cannot tell yet
  // whether more bindings (then an arrow + body) follow or whether the
  // lambda terminates with an empty-bracket absurd pattern. GLR-explore
  // both `lambda` and `_absurd_lambda_bindings`.
  conflicts: $ => [
    [$.lambda, $._absurd_lambda_bindings],
    [$._binding_ids_and_absurds, $.lambda_clause_absurd],
    // `let <decl> where …` inside a do-block: GLR-explore whether the trailing
    // `where` attaches to the let's declaration or starts a new clause after the
    // let closes.  (Surfaced by moving the let's `_dedent` close before the
    // optional `in`-body.)
    [$.function],
    [$.data, $.data_signature],
    [$.record, $.record_signature],
    [$.with_expressions],
    [$.record_declarations_block, $.record_where_named],
    [$._declaration_block, $.record_declarations_block],
    [$._expr2_without_let],
    [$.attributes],
    [$.untyped_binding],
    [$.bid, $.as_pattern],
  ],

  // Globally reserved words. Tree-sitter's keyword extraction is normally
  // per-state (a string token is treated as a keyword only at parse states
  // where it is grammatically expected) — which means a keyword that is
  // valid only inside a deeply nested rule (like `unfolding` inside
  // `opaque`, or `with` inside an LHS) would still parse as an identifier
  // in unrelated positions. Listing them here forces the lexer to always
  // emit the keyword token.
  reserved: {
    global: $ => [
      // `?` is the interaction-hole atom; it's its own one-token
      // expression and cannot be used as an identifier (e.g.
      // `(? : Set)` is not a valid binder).
      '?',
      // `quote`, `quoteTerm`, and `unquote` are intentionally NOT
      // listed — agda treats them as keywords too, but in our grammar
      // they double as Expr3 atoms (`f = quote`, `f = unquote` parse
      // as bare reflection-node expressions; the type-checker rejects
      // them later if no argument is supplied). Listing them as
      // reserved would break those uses without giving us anything in
      // return.
      'abstract',
      // 'as' is intentionally NOT globally reserved — Agda treats it as a keyword
      // only in `import`/`open import` rename clauses and as a plain identifier
      // elsewhere (`f (x ∷ as) = g as`). It is handled by the `_as_rename` external
      // instead. (`open A as B` parses as `open A` applied to args `as B`; rejecting
      // it is left to a post-parse phase.)
      'codata',
      'coinductive',
      'constructor',
      'data',
      'eta-equality',
      'field',
      'hiding',
      'import',
      'in',
      'inductive',
      'infix',
      'infixl',
      'infixr',
      'instance',
      'interleaved',
      'macro',
      'module',
      'mutual',
      'no-eta-equality',
      'opaque',
      'open',
      'overlap',
      'pattern',
      'postulate',
      'primitive',
      'private',
      'public',
      'renaming',
      'rewrite',
      'syntax',
      // 'to' is intentionally NOT globally reserved — Agda only treats it as
      // a keyword inside `renaming (A to B)` clauses and allows it as a
      // regular identifier everywhere else (e.g. `field to : A → B`).
      // Tree-sitter's keyword-extraction (word: $ => $.id) handles the
      // disambiguation: the literal 'to' wins only when the renaming rule
      // expects it; elsewhere `to` matches as $.id.
      'unfolding',
      'unquoteDecl',
      'unquoteDef',
      'using',
      'variable',
      'where',
      'with',
    ],
  },

  rules: {
    source_file: $ => choice(
      block($, $._declaration),
      repeat(seq($._declaration, choice($._newline, ';'))),
    ),


    // //////////////////////////////////////////////////////////////////////
    // Constants
    // //////////////////////////////////////////////////////////////////////

    _FORALL: _ => choice('forall', '∀'),
    _ARROW: _ => choice('->', '→'),
    _LAMBDA: _ => choice(token(prec(-1, '\\')), 'λ'),
    _ELLIPSIS: _ => choice('...', '…'),

    // //////////////////////////////////////////////////////////////////////
    // Top-level Declarations
    // //////////////////////////////////////////////////////////////////////

    _declaration_block: $ => block($, $._declaration),

    _declaration: $ => choice(
      $.fields,
      $.function,
      $.data,
      $.data_signature,
      $.record,
      $.record_signature,
      $.infix,
      $.generalize,
      $.mutual,
      $.interleaved_mutual,
      $.abstract,
      $.private,
      $.instance,
      $.macro,
      $.postulate,
      $.primitive,
      $.open,
      alias($._import_declaration, $.import),
      $.module_macro,
      $.module,
      $.pragma,
      $.syntax,
      $.pattern,
      $.unquote_decl,
      $.opaque,
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Field
    // //////////////////////////////////////////////////////////////////////

    fields: $ => seq(
      'field',
      optional($._signature_block),
    ),

    _signature_block: $ => block($, $.signature),

    signature: $ => choice(
      seq(
        optional('overlap'),
        $._modal_arg_ids,
        ':',
        $.expr,
      ),
      // Instance field signatures `{{name}} : Type` are spelled out here so that
      // '{{' / '⦃' are direct first-tokens of this state. The brace_double inside
      // _arg_id doesn't propagate its '{{' first-token through the inline-rule
      // chain, so without these the lexer consumes only a single '{' and the
      // second '}' becomes an ERROR node.
      prec(1, seq('{{', $._maybe_dotted_ids, '}}', ':', $.expr)),
      prec(1, seq('⦃', $._maybe_dotted_ids, '⦄', ':', $.expr)),
      seq(
        'instance',
        $._signature_block,
      ),
    ),

    _modal_arg_ids: $ => seq(repeat($.attribute), $._arg_ids),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Functions
    // //////////////////////////////////////////////////////////////////////

    // Split into two alternatives — declaration (`:`) and definition (`=`) — only
    // so the declaration's LHS can be aliased to `function_name`.
    function: $ => choice(
      seq(
        optional($.attributes),
        alias($.lhs_decl, $.lhs),
        alias(optional($.rhs_decl), $.rhs),
        optional($.where),
      ),
      seq(
        optional($.attributes),
        alias($.lhs_defn, $.lhs),
        alias(optional($.rhs_defn), $.rhs),
        optional($.where),
      ),
    ),

    // The rewrite / using / with clauses on an LHS may chain freely, e.g.
    // `f x rewrite p with r using s ← e = body`.
    lhs_decl: $ => seq(
      alias($._with_exprs, $.function_name),
      repeat($._whs_clause),
    ),
    lhs_defn: $ => prec(1, seq(
      $._with_exprs,
      repeat($._whs_clause),
    )),

    _whs_clause: $ => choice(
      $.rewrite_equations,
      $.using_clause,
      $.with_expressions,
    ),

    rhs_decl: $ => seq(':', $.expr),
    rhs_defn: $ => seq('=', $.expr),

    with_expressions: $ => seq(
      'with',
      $.expr,
      optional(seq('in', $.id)),
      repeat(seq('|', $.expr, optional(seq('in', $.id)))),
    ),

    rewrite_equations: $ => seq('rewrite', $._with_exprs),

    using_clause: $ => seq(
      'using',
      field('pattern', $.expr),
      $._LEFTARROW,
      field('rhs', $.expr),
    ),

    _LEFTARROW: _ => choice('<-', '←'),

    where: $ => seq(
      optional(seq(
        'module',
        optional($.attributes),
        $.bid,
      )),
      'where',
      optional(choice($._declaration_block, $._declaration)),
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Data
    // //////////////////////////////////////////////////////////////////////

    data_name: $ => alias(choice($.id, '_'), 'data_name'),

    data: $ => seq(
      choice('data', 'codata'),
      optional($.attributes),
      $.data_name,
      optional($._typed_untyped_bindings),
      optional(seq(':', $.expr)),
      'where',
      optional(choice($._declaration_block, $._declaration)),
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Data Signature
    // //////////////////////////////////////////////////////////////////////

    data_signature: $ => seq(
      'data',
      optional($.attributes),
      $.data_name,
      optional($._typed_untyped_bindings),
      ':',
      $.expr,
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Record
    // //////////////////////////////////////////////////////////////////////

    record: $ => seq(
      'record',
      optional($.attributes),
      alias($._atom_no_curly, $.record_name),
      optional($._typed_untyped_bindings),
      optional(seq(':', $.expr)),
      $.record_declarations_block,
    ),

    record_declarations_block: $ => seq(
      'where',
      optional(indent($,
        // Directives, one or more per line; `;` may separate them on a single line
        // and may also separate a trailing directive from a declaration. Once a
        // declaration appears, the rest of the `;`-list are declarations.
        repeat(seq(
          $._record_directive,
          repeat(seq(';', choice($._record_directive, $._declaration))),
          $._newline,
        )),
        repeat(seq($._declaration, choice($._newline, ';'))),
      )),
    ),

    _record_directive: $ => choice(
      $.record_constructor,
      $.record_constructor_instance,
      $.record_induction,
      $.record_eta,
      $.record_pattern,
    ),
    record_constructor: $ => seq('constructor', $.id),

    record_constructor_instance: $ => seq(
      'instance',
      block($, $.record_constructor),
    ),

    record_induction: _ => choice(
      'inductive',
      'coinductive',
    ),

    record_eta: _ => choice(
      'eta-equality',
      'no-eta-equality',
    ),

    record_pattern: _ => 'pattern',


    // //////////////////////////////////////////////////////////////////////
    // Declaration: Record Signature
    // //////////////////////////////////////////////////////////////////////

    record_signature: $ => seq(
      'record',
      optional($.attributes),
      alias($._atom_no_curly, $.record_name),
      optional($._typed_untyped_bindings),
      ':',
      $.expr,
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Infix
    // //////////////////////////////////////////////////////////////////////

    infix: $ => seq(
      $.fixity,
      repeat1($.bid),
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Generalize
    // //////////////////////////////////////////////////////////////////////

    generalize: $ => seq(
      'variable',
      optional($._signature_block),
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Mutual
    // //////////////////////////////////////////////////////////////////////

    mutual: $ => seq(
      'mutual',
      optional(choice($._declaration_block, $._declaration)),
    ),

    interleaved_mutual: $ => seq(
      'interleaved',
      'mutual',
      optional(choice($._declaration_block, $._declaration)),
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Opaque / Unfolding
    // //////////////////////////////////////////////////////////////////////

    opaque: $ => seq(
      'opaque',
      choice(
        // `unfolding` must come first; if declarations precede it, error recovery
        // wraps them in ERROR and resumes at the first `unfolding`.
        indent($,
          seq(
            repeat1(seq($.unfolding, $._newline)),
            repeat(seq($._declaration, $._newline)),
          ),
        ),
        // inline `opaque unfolding …`
        seq(
          $.unfolding,
          optional($._declaration_block),
        ),
        optional($._declaration_block),
      ),
    ),

    unfolding: $ => seq(
      'unfolding',
      repeat(alias($._qid, $.qid)),
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Abstract
    // //////////////////////////////////////////////////////////////////////

    abstract: $ => seq(
      'abstract',
      optional(choice($._declaration_block, $._declaration)),
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Private
    // //////////////////////////////////////////////////////////////////////

    private: $ => seq(
      'private',
      optional(choice($._declaration_block, $._declaration)),
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Instance
    // //////////////////////////////////////////////////////////////////////

    instance: $ => seq(
      'instance',
      optional(choice($._declaration_block, $._declaration)),
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Macro
    // //////////////////////////////////////////////////////////////////////

    macro: $ => seq(
      'macro',
      optional(choice($._declaration_block, $._declaration)),
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Postulate
    // //////////////////////////////////////////////////////////////////////

    postulate: $ => seq(
      'postulate',
      optional(choice($._declaration_block, $._declaration)),
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Primitive
    // //////////////////////////////////////////////////////////////////////

    primitive: $ => seq(
      'primitive',
      optional($._primitive_declaration_block),
    ),

    // Primitive blocks allow type signatures, private, and instance sub-blocks
    // but not arbitrary function declarations (avoids type_sig/function ambiguity).
    _primitive_declaration_block: $ => block($, choice(
      $.type_signature,
      $.private,
      $.instance,
    )),

    type_signature: $ => seq(
      optional($.attributes),
      $._field_names,
      ':',
      $.expr,
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Open
    // //////////////////////////////////////////////////////////////////////


    // `open` itself takes no `as`-clause (`open A as B` is rejected upstream); only
    // `open import M args as N directives` is valid, where the `as_clause` attaches
    // to the inner `import`-with-args sub-tree.
    open: $ => seq(
      'open',
      choice(
        seq($.import, optional($._atoms), optional($.as_clause)),
        seq($.module_name, optional($._atoms)),
      ),
      optional($._import_directives),
    ),

    import: $ => seq('import', $.module_name),

    // Top-level `import M args` is rejected by Agda as "useless" unless renamed, so
    // an `as`-clause is required when args are present. `as` always comes after the
    // args and before any directives.
    _import_declaration: $ => seq(
      'import',
      $.module_name,
      choice(
        seq($._atoms, optional($.as_clause), optional($._import_directives)),
        seq(optional($.as_clause), optional($._import_directives)),
      ),
    ),

    as_clause: $ => seq($._as_rename, alias(choice(seq('.', $._qid), $._qid, '_'), $.module_name)),


    module_name: $ => $._qid,

    _import_directives: $ => repeat1($.import_directive),
    import_directive: $ => choice(
      'public',
      seq('using', '(', $._comma_import_names, ')'),
      seq('hiding', '(', $._comma_import_names, ')'),
      seq('renaming', '(', sepR(';', $.renaming), ')'),
      seq('using', '(', ')'),
      seq('hiding', '(', ')'),
      seq('renaming', '(', ')'),
    ),

    _comma_import_names: $ => sepR(';', $._import_name),

    renaming: $ => seq(
      optional('module'),
      $.id,
      'to',
      optional($.fixity),
      $.id,
    ),

    fixity: $ => seq(
      choice('infix', 'infixl', 'infixr'),
      alias(token(/\-?[0-9]+(\.[0-9]+)?/), $.integer),
    ),

    _import_name: $ => seq(
      optional('module'), $.id,
    ),


    // //////////////////////////////////////////////////////////////////////
    // Declaration: Module Macro
    // //////////////////////////////////////////////////////////////////////

    // The LHS bound name should be unqualified (`module Sort.Nat = …` is rejected
    // upstream), but we use `_qid` here: it shares the prefix `module Name (binds)`
    // with `$.module`, and an `id`-vs-`_qid` split won't GLR-resolve until the
    // distinguishing `=`/`where`. (The RHS source module may be qualified.)
    module_macro: $ => seq(
      choice(
        seq('module', optional($.attributes), alias($._qid, $.module_name)),
        seq('open', 'module', optional($.attributes), alias($._qid, $.module_name)),
      ),
      optional($._typed_untyped_bindings),
      '=',
      $.module_application,
      repeat($.import_directive),
    ),

    module_application: $ => seq(
      $.module_name,
      choice(
        prec(1, brace_double($._ELLIPSIS)),
        optional($._atoms),
      ),
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Module
    // //////////////////////////////////////////////////////////////////////

    module: $ => seq(
      'module',
      optional($.attributes),
      alias(choice($._qid, '_'), $.module_name),
      optional($._typed_untyped_bindings),
      'where',
      optional(choice($._declaration_block, $._declaration)),
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Pragma
    // //////////////////////////////////////////////////////////////////////

    // The pragma name is intentionally NOT validated (any content between `{-#`
    // and `#-}` is accepted) — Agda restricts it to a fixed, frequently-growing
    // list, and mirroring that here would be churn for little gain.
    pragma: _ => token(seq(
      '{-#',
      repeat(choice(
        // one level of nesting, so a FOREIGN GHC pragma can hold a nested
        // `{-# LANGUAGE … #-}`
        seq('{-#', repeat(choice(/[^#]/, /#[^-]/, /#\-[^}]/)), '#-}'),
        /[^#{]/,
        /\{[^-]/,
        /\{-[^#]/,
        /#[^-]/,
        /#\-[^}]/,
      )),
      '#-}',
    )),

    catchall_pragma: _ => seq('{-#', 'CATCHALL', '#-}'),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Syntax
    // //////////////////////////////////////////////////////////////////////

    syntax: $ => seq(
      'syntax',
      $.id,
      optional($.hole_names),
      '=',
      repeat1($.id),
    ),

    hole_names: $ => repeat1($.hole_name),
    hole_name: $ => choice(
      $._simple_top_hole,
      brace($._simple_hole),
      brace_double($._simple_hole),
      brace($.id, '=', $._simple_hole),
      brace_double($.id, '=', $._simple_hole),
    ),

    _simple_top_hole: $ => choice(
      $.id,
      paren($._LAMBDA, repeat1($.bid), $._ARROW, $.id),
    ),

    _simple_hole: $ => choice(
      $.id,
      seq($._LAMBDA, repeat1($.bid), $._ARROW, $.id),
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Pattern Synonym
    // //////////////////////////////////////////////////////////////////////

    pattern: $ => seq(
      'pattern',
      $.id,
      optional($._lambda_bindings),
      '=',
      $.expr,
    ),

    // //////////////////////////////////////////////////////////////////////
    // Declaration: Unquoting declarations
    // //////////////////////////////////////////////////////////////////////

    unquote_decl: $ => choice(
      seq('unquoteDecl', '=', $.expr),
      seq('unquoteDecl', $._ids, '=', $.expr),
      seq('unquoteDef', $._ids, '=', $.expr),
      seq('unquoteDecl', 'data', $.id, '=', $.expr),
      seq(
        'unquoteDecl', 'data', $.id,
        'constructor', $._ids,
        '=', $.expr,
      ),
    ),

    // //////////////////////////////////////////////////////////////////////
    // Names
    // //////////////////////////////////////////////////////////////////////

    // identifier: http://wiki.portal.chalmers.se/agda/pmwiki.php?n=ReferenceManual.Names
    id: _ => /([^\s;\\.\"\(\)\{\}@\'\\_]|\\[^\sa-zA-Z\(\)\{\}]|_[^\s;\.\"\(\)\{\}@])[^\s;\.\"\(\)\{\}@]*/,

    // qualified identifier: http://wiki.portal.chalmers.se/agda/pmwiki.php?n=ReferenceManual.Names
    _qid: $ => prec.left(
      choice(
        // eslint-disable-next-line max-len
        alias(/(([^\s;\.\"\(\)\{\}@\'\\_]|\\[^\sa-zA-Z]|_[^\s;\.\"\(\)\{\}@])[^\s;\.\"\(\)\{\}@]*\.)+([^\s;\.\"\(\)\{\}@\'\\_]|\\[^\sa-zA-Z]|_[^\s;\.\"\(\)\{\}@])[^\s;\.\"\(\)\{\}@]*/, $.qid),
        alias($.id, $.qid),
      ),
    ),

    bid: $ => alias(choice('_', $.id), 'bid'),

    _ids: $ => repeat1($.id),

    _field_name: $ => alias($.id, $.field_name),
    _field_names: $ => repeat1($._field_name),

    _maybe_dotted_id: $ => maybeDotted($._field_name),
    _maybe_dotted_ids: $ => repeat1($._maybe_dotted_id),

    _arg_ids: $ => repeat1($._arg_id),
    _arg_id: $ => choice(
      $._maybe_dotted_id,

      brace($._maybe_dotted_ids),
      brace_double($._maybe_dotted_ids),

      seq('.', brace($._field_names)),
      seq('.', brace_double($._field_names)),

      seq('..', brace($._field_names)),
      seq('..', brace_double($._field_names)),
    ),

    // The field-assignment alternatives are prec(-1) so that `_qid =` in an
    // expression context reduces to `_expr_or_attr` rather than shifting `=` into
    // a field assignment. The plain `_application` alternative carries no extra
    // precedence so the GLR `[lambda, lambda_extended_or_absurd]` conflict can
    // explore both the binding and the clause path.
    _binding_ids_and_absurds: $ => choice(
      $._application,
      prec(-1, seq($._qid, '=', $._qid)),
      prec(-1, seq($._qid, '=', '_')),
    ),

    attribute: $ => seq('@', choice($._expr_or_attr, 'rewrite')),
    attributes: $ => repeat1($.attribute),

    // //////////////////////////////////////////////////////////////////////
    // Expressions (terms and types)
    // //////////////////////////////////////////////////////////////////////

    expr: $ => choice(
      seq($._typed_bindings, $._ARROW, $.expr),
      seq(optional($.attributes), $._atoms, $._ARROW, $.expr),
      seq($._with_exprs, '=', $.expr),
      prec(-1, $._with_exprs),
    ),
    stmt: $ => choice(
      seq($._typed_bindings, $._ARROW, $.expr),
      seq(optional($.attributes), $._atoms, $._ARROW, $.expr),
      seq($._with_exprs, '=', $.expr),
      seq(
        field('pattern', $._with_exprs),
        $._LEFTARROW,
        field('rhs', $.expr),
      ),
      prec(-1, $._with_exprs_stmt),
    ),

    _with_exprs: $ => seq(
      repeat(seq($._atoms, '|')),
      $._application,
    ),
    _with_exprs_stmt: $ => seq(
      repeat(seq($._atoms, '|')),
      $._application_stmt,
    ),

    _expr_or_attr: $ => choice(
      $.literal,
      $._qid,
      paren($.expr),
    ),

    _application: $ => seq(
      optional($._atoms),
      $._expr2,
    ),
    _application_stmt: $ => seq(
      optional($._atoms),
      $._expr2_stmt,
    ),

    _expr2_without_let: $ => choice(
      $.lambda,
      alias($.lambda_extended_or_absurd, $.lambda),
      $.forall,
      $.do,
      prec(-1, $.atom),
      seq('quoteGoal', $.id, 'in', $.expr),
      seq('tactic', $._atoms),
      seq('tactic', $._atoms, '|', $._with_exprs),
    ),
    _expr2: $ => choice(
      $._expr2_without_let,
      $.let,
    ),
    _expr2_stmt: $ => choice(
      $._expr2_without_let,
      alias($.let_in_do, $.let),
    ),

    atom: $ => choice(
      $._atom_curly,
      $._atom_no_curly,
    ),
    _atoms: $ => repeat1($.atom),

    // As-pattern `x@p` (Agda's `As` constructor, Parser.y `Id '@' Expr3`).
    as_pattern: $ => seq($.id, '@', $.atom),

    _atom_curly: $ => brace(optional($.expr)),

    _atom_no_curly: $ => choice(
      '_',
      '?',
      $.SetN,
      'quote',
      'quoteTerm',
      'quoteContext',
      'unquote',
      $.PropN,
      brace_double($.expr),
      seq($._open_idiom, optional($.expr), '|)'),
      seq('⦇', optional($.expr), '⦈'),
      $._empty_idiom,
      $.hole,
      seq('(', ')'),
      seq('{{', '}}'),
      seq('⦃', '⦄'),
      $.as_pattern,
      seq('.', paren($.attributes, $.atom)),
      seq('.', $.atom),
      seq('..', $.atom),
      $.record_assignments,
      alias($.field_assignments, $.record_assignments),
      alias($.record_where_named, $.record_assignments),
      $.record_where_expr,
      $._ELLIPSIS,
      $._expr_or_attr,
    ),

    forall: $ => seq($._FORALL, $._typed_untyped_bindings, $._ARROW, $.expr),

    // The block-closing `$._dedent` sits BEFORE the optional `in`-body, matching
    // Agda's `vclose` position in `Expr2 : 'let' Declarations LetBody`. The scanner
    // emits one unified `_dedent`, driven by column-offside or by `in`/`)`/`}`
    // lookahead (emulating Agda's `error{%popBlock}`). Consuming the close right
    // after the declarations stops it drifting past the body and stealing an
    // enclosing block's DEDENT.
    let: $ => prec.right(seq(
      'let',
      optional(choice($._indent, $._let_indent)),
      repeat(seq($._declaration, choice($._newline, ';'))),
      $._declaration,
      optional(choice($._newline, ';')),
      optional($._dedent),
      $._let_body,
    )),

    // As `let`, but the `in`-body is optional (a `let` may be a do-statement alone).
    let_in_do: $ => prec.right(seq(
      'let',
      optional(choice($._indent, $._let_indent)),
      repeat(seq($._declaration, choice($._newline, ';'))),
      $._declaration,
      optional(choice($._newline, ';')),
      optional($._dedent),
      optional($._let_body),
    )),

    _let_body: $ => seq(
      'in',
      $.expr,
    ),

    // prec.dynamic(1): when both `lambda` and `lambda_extended_or_absurd` accept
    // the same tokens (e.g. `λ {b} hb → body` inside `let…in (…)`), prefer this
    // binding form over the clause form.
    lambda: $ => prec.dynamic(1, seq(
      $._LAMBDA,
      optional($.attributes),
      $._lambda_bindings,
      $._ARROW,
      $.expr,
    )),

    // Bindings for `λ … → e`. A trailing empty-bracket (absurd) pattern is NOT
    // allowed here — an absurd binding can't be followed by a body — so that case
    // lives in `lambda_extended_or_absurd` / `_absurd_lambda_bindings` instead.
    _lambda_bindings: $ => repeat1($._typed_untyped_binding),

    lambda_extended_or_absurd: $ => seq(
      $._LAMBDA,
      optional($.attributes),
      choice(
        brace($.lambda_clause),
        brace($._lambda_clauses),
        seq('where', $._lambda_clauses), // inline `;`-separated clauses
        seq('where', $._lambda_where_block), // INDENT/DEDENT clause block
        $._absurd_lambda_bindings,
      ),
    ),

    _absurd_lambda_bindings: $ => seq(
      repeat($._typed_untyped_binding),
      choice(
        seq('(', ')'),
        seq('{', '}'),
        seq('{{', '}}'),
        seq('⦃', '⦄'),
      ),
    ),

    // Uses $._lambda_newline (not $._newline) as the clause separator so that a
    // do-block INDENT inside `g (λ where …)` keeps PAREN_FLAG (NEWLINE stays
    // invalid at the do-INDENT push, marking it as paren-enclosed).
    _lambda_where_block: $ => seq(
      choice($._indent, $._lam_where_indent),
      $._lambda_clause_maybe_absurd,
      repeat(seq(choice($._lambda_newline, ';'), $._lambda_clause_maybe_absurd)),
      $._dedent,
    ),

    // prec.right so that where `;` is also a declaration separator in the outer
    // context, the lambda greedily extends its clause list before the outer rule
    // can claim the `;`.
    _lambda_clauses: $ => prec.right(seq(
      repeat(seq($._lambda_clause_maybe_absurd, ';')),
      $._lambda_clause_maybe_absurd,
    )),

    _lambda_clause_maybe_absurd: $ => prec.right(choice(
      $.lambda_clause_absurd,
      $.lambda_clause,
    )),

    lambda_clause_absurd: $ => seq(
      optional($.catchall_pragma),
      $._application,
    ),

    lambda_clause: $ => seq(
      optional($.catchall_pragma),
      optional($._atoms),
      $._ARROW,
      $.expr,
    ),

    do: $ => seq('do',
      seq(
        choice($._indent, $._do_indent),
        repeat1(seq($._do_stmt, choice($._newline, ';'))),
        $._dedent,
      ),
    ),

    _do_stmt: $ => seq(
      $.stmt,
      optional($.do_where),
    ),

    do_where: $ => seq(
      'where',
      $._lambda_clauses,
    ),

    record_assignments: $ => seq(
      'record',
      brace(optional($._record_assignments)),
    ),

    record_where_expr: $ => seq(
      'record',
      'where',
      optional($._declaration_block),
    ),

    field_assignments: $ => seq(
      'record',
      $._atom_no_curly,
      brace(optional($._field_assignments)),
    ),

    record_where_named: $ => seq(
      'record',
      $._atom_no_curly,
      'where',
      optional($._declaration_block),
    ),

    _record_assignments: $ => seq(
      repeat(seq($._record_assignment, ';')),
      $._record_assignment,
    ),


    _field_assignments: $ => seq(
      repeat(seq($.field_assignment, ';')),
      $.field_assignment,
    ),

    _record_assignment: $ => choice(
      $.field_assignment,
      $.module_assignment,
    ),

    field_assignment: $ => choice(
      seq(alias($.id, $.field_name), '=', $.expr),
      seq('.', alias($.id, $.field_name), $._ARROW, $.expr),
    ),

    module_assignment: $ => seq(
      $.module_name,
      optional($._atoms),
      optional($._import_directives),
    ),


    // //////////////////////////////////////////////////////////////////////
    // Bindings
    // //////////////////////////////////////////////////////////////////////

    _typed_bindings: $ => repeat1($.typed_binding),
    typed_binding: $ => choice(
      maybeDotted(choice(
        paren($._application, ':', $.expr),
        brace($._binding_ids_and_absurds, ':', $.expr),
        brace_double($._binding_ids_and_absurds, ':', $.expr),
        brace($.attributes, $._binding_ids_and_absurds, ':', $.expr),
        brace_double($.attributes, $._binding_ids_and_absurds, ':', $.expr),
      )),
      paren($.attributes, $._application, ':', $.expr),
      brace($.attributes, $._binding_ids_and_absurds, ':', $.expr),
      brace_double($.attributes, $._binding_ids_and_absurds, ':', $.expr),
      paren($.open),
      paren(seq(
        'let',
        optional($._indent),
        repeat(seq($._declaration, choice($._newline, ';'))),
        $._declaration,
        optional(choice($._newline, ';')),
        optional($._dedent),
      )),
    ),

    _typed_untyped_bindings: $ => repeat1($._typed_untyped_binding),
    _typed_untyped_binding: $ => choice(
      $.untyped_binding,
      $.typed_binding,
    ),

    untyped_binding: $ => choice(
      maybeDotted(choice(
        $.bid,
        brace($._binding_ids_and_absurds),
        brace_double($._binding_ids_and_absurds),
        brace($.attributes, $._binding_ids_and_absurds),
        brace_double($.attributes, $._binding_ids_and_absurds),
      )),
      paren($._binding_ids_and_absurds),
      paren($.attributes, $._binding_ids_and_absurds),
      brace($.attributes, $._binding_ids_and_absurds),
      brace_double($.attributes, $._binding_ids_and_absurds),
      $.as_pattern,
      seq($.attributes, $.bid),
      seq('.', paren($.attributes, $.atom)),
    ),

    // //////////////////////////////////////////////////////////////////////
    // Literals
    // //////////////////////////////////////////////////////////////////////

    integer: _ => integer,
    // The string-gap alternative `\<whitespace>+\` (Haskell-style line
    // continuation, may span newlines) must precede `\\.` because `.` doesn't
    // match a newline.
    string: _ => /\"([^\"\\]|\\[ \t\r\n]+\\|\\.)*\"/,
    char: _ => token(seq(
      '\'',
      choice(
        /[^'\\]/,
        /\\[abfnrtvz\\'0]/,
        /\\x[0-9a-fA-F]+/,
        /\\[0-9]+/,
        // named control characters; SOH must precede SO so the longest matches
        /\\(NUL|SOH|STX|ETX|EOT|ENQ|ACK|BEL|BS|HT|LF|VT|FF|CR|SO|SI|DLE|DC1|DC2|DC3|DC4|NAK|SYN|ETB|CAN|EM|SUB|ESC|FS|GS|RS|US|SP|DEL)/,
      ),
      '\'',
    )),
    literal: $ => choice(
      integer,
      $.string,
      $.char,
    ),

    hole: _ => token(seq(
      '{!',
      repeat(choice(
        /[^!]/,
        /![^}]/,
      )),
      '!}',
    )),

    // //////////////////////////////////////////////////////////////////////
    // Comment
    // //////////////////////////////////////////////////////////////////////

    comment: _ => token(prec(100, seq('--', /[^\n]*/))),

    SetN: $ => prec.right(2, seq('Set', optional($.atom))),

    PropN: $ => prec.right(2, seq('Prop', optional($.atom))),

  },
});


// //////////////////////////////////////////////////////////////////////
// Generic combinators
// //////////////////////////////////////////////////////////////////////

/**
 * Creates a rule to match one or more of the rules separated by `sep`.
 *
 * @param {RuleOrLiteral} sep
 *
 * @param {RuleOrLiteral} rule
 *
 * @returns {SeqRule}
 */
function sepR(sep, rule) {
  return seq(rule, repeat(seq(sep, rule)));
}

/**
 * Creates a rule that requires indentation before and dedentation after.
 *
 * @param {GrammarSymbols<any>} $
 *
 * @param {RuleOrLiteral[]} rule
 *
 * @returns {SeqRule}
 */
function indent($, ...rule) {
  return seq(
    $._indent,
    ...rule,
    $._dedent,
  );
}

// 1 or more $RULE ending with a NEWLINE
/**
 * Creates a rule that uses an indentation block, where each line is a rule.
 * The indentation is required before and dedentation is required after.
 *
 * @param {GrammarSymbols<any>} $
 *
 * @param {RuleOrLiteral} rules
 *
 * @returns {SeqRule}
 */
function block($, rules) {
  return indent($, repeat1(seq(rules, choice($._newline, ';'))));
}

// //////////////////////////////////////////////////////////////////////
// Language-specific combinators
// //////////////////////////////////////////////////////////////////////

/**
 * Creates a rule that matches a rule with a dot or two dots in front.
 *
 * @param {RuleOrLiteral} rule
 *
 * @returns {ChoiceRule}
 */
function maybeDotted(rule) {
  return choice(
    rule, // Relevant
    seq('.', rule), // Irrelevant
    seq('..', rule), // NonStrict
  );
}

/**
 * Flattens an array of arrays.
 *
 * @param {Array<Array<Array<string>>>} arrOfArrs
 *
 * @returns {Array<Array<string>>}
 */
function flatten(arrOfArrs) {
  return arrOfArrs.reduce((res, arr) => [...res, ...arr], []);
}

/**
 * A callback function that takes a left and right string and returns a rule.
 *
 * @callback encloseWithCallback
 * @param {string} left
 * @param {string} right
 * @returns {RuleOrLiteral}
 * @see encloseWith
 * @see enclose
 */

/**
 * Creates a rule that matches a sequence of rules enclosed by a pair of strings.
 *
 * @param {encloseWithCallback} fn
 *
 * @param {Array<Array<Array<string>>>} pairs
 *
 * @returns {ChoiceRule}
 */
function encloseWith(fn, ...pairs) {
  return choice(...flatten(pairs).map(([left, right]) => fn(left, right)));
}

/**
 *
 * @param {RuleOrLiteral} expr
 *
 * @param {Array<Array<Array<string>>>} pairs
 *
 * @returns {ChoiceRule}
 */
function enclose(expr, ...pairs) {
  return encloseWith((left, right) => seq(left, expr, right), ...pairs);
}

/**
 * Creates a rule that matches a sequence of rules enclosed by `(` and `)`.
 *
 * @param {RuleOrLiteral[]} rules
 *
 * @returns {ChoiceRule}
 */
function paren(...rules) {
  return enclose(seq(...rules), PAREN);
}

/**
 * Creates a rule that matches a sequence of rules enclosed by `{` and `}`.
 *
 * @param {RuleOrLiteral[]} rules
 *
 * @returns {ChoiceRule}
 */
function brace(...rules) {
  return enclose(seq(...rules), BRACE1);
}

/**
 * Creates a rule that matches a sequence of rules enclosed by `{{` and `}}`.
 *
 * @param {RuleOrLiteral[]} rules
 *
 * @returns {ChoiceRule}
 */
function brace_double(...rules) {
  return enclose(seq(...rules), BRACE2);
}
