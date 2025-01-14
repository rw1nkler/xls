// Copyright 2023 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/dslx/fmt/ast_fmt.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "absl/types/variant.h"
#include "xls/common/logging/logging.h"
#include "xls/common/visitor.h"
#include "xls/dslx/channel_direction.h"
#include "xls/dslx/fmt/pretty_print.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/comment_data.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/frontend/token.h"
#include "xls/ir/format_strings.h"

namespace xls::dslx {
namespace {

// Forward decls.
DocRef Fmt(const TypeAnnotation& n, const Comments& comments, DocArena& arena);
DocRef Fmt(const ColonRef& n, const Comments& comments, DocArena& arena);
DocRef FmtExprOrType(const ExprOrType& n, const Comments& comments,
                     DocArena& arena);
DocRef Fmt(const NameDefTree& n, const Comments& comments, DocArena& arena);

static DocRef FmtExprPtr(const Expr* n, const Comments& comments,
                         DocArena& arena) {
  XLS_CHECK(n != nullptr);
  return Fmt(*n, comments, arena);
}

enum class Joiner : uint8_t {
  kCommaSpace,
  kCommaBreak1,

  // Separates via a comma and break1, but groups the element with its
  // delimiter. This is useful when we're packing member elements that we want
  // to be reflowed across lines.
  //
  // Note that, in this mode, if we span multiple lines, we'll put a trailing
  // comma as well.
  kCommaBreak1AsGroup,

  kSpaceBarBreak,
  kHardLine,
};

// Helper for doing a "join via comma space" pattern with doc refs.
//
// This elides the "joiner" being present after the last item.
template <typename T>
DocRef FmtJoin(
    absl::Span<const T> items, Joiner joiner,
    const std::function<DocRef(const T&, const Comments&, DocArena&)>& fmt,
    const Comments& comments, DocArena& arena) {
  std::vector<DocRef> pieces;
  for (size_t i = 0; i < items.size(); ++i) {
    const T& item = items[i];
    pieces.push_back(fmt(item, comments, arena));
    if (i + 1 != items.size()) {
      switch (joiner) {
        case Joiner::kCommaSpace:
          pieces.push_back(arena.comma());
          pieces.push_back(arena.space());
          break;
        case Joiner::kCommaBreak1:
          pieces.push_back(arena.comma());
          pieces.push_back(arena.break1());
          break;
        case Joiner::kCommaBreak1AsGroup: {
          DocRef member = pieces.back();
          pieces.pop_back();
          std::vector<DocRef> this_pieces;
          if (i != 0) {
            this_pieces.push_back(arena.break1());
          }
          this_pieces.push_back(member);
          this_pieces.push_back(arena.comma());
          pieces.push_back(ConcatNGroup(arena, this_pieces));
          break;
        }
        case Joiner::kSpaceBarBreak:
          pieces.push_back(arena.space());
          pieces.push_back(arena.bar());
          pieces.push_back(arena.break1());
          break;
        case Joiner::kHardLine:
          pieces.push_back(arena.hard_line());
          break;
      }
    } else {  // last member, no trailing delimiter
      if (joiner == Joiner::kCommaBreak1AsGroup && i != 0) {
        // Note: we only want to put a leading space in front of the last
        // element if the last element is not also the first element.
        pieces.back() = ConcatNGroup(arena, {arena.break1(), pieces.back()});

        // With this pattern if we're in break mode (implying we spanned
        // multiple lines), we allow a trailing comma.
        pieces.push_back(arena.MakeFlatChoice(arena.empty(), arena.comma()));
      }
    }
  }
  return ConcatN(arena, pieces);
}

// Returns all the comment data that's contained within `node_span` of the AST
// node, but knocking out comment data that's within block expressions contained
// under node.
//
// For example, in:
//
//    let x = {
//        // Comment in here.
//        let y = u32:42;
//        // This is not multiple inline comments.
//        y
//    };
//
// we want to "knock out" the comments contained within the block expression as
// pertaining to the Let node.
//
// Implementation note: we assume this is a small vector (that will also
// typically will go un-modified) so we just do linear traversals.
static std::vector<const CommentData*> GetCommentsForNode(
    const AstNode& node, const Span& node_span, const Comments& comments) {
  std::vector<const CommentData*> all = comments.GetComments(node_span);

  auto remove_comments_under = [&](const Span& span) {
    // Note: in the typical case we expect that there are no comments under the
    // given "span", so we do a simple/readable test for it up front.
    if (!std::any_of(all.begin(), all.end(), [&](const CommentData* cd) {
          return span.Contains(cd->span);
        })) {
      return;
    }

    std::vector<const CommentData*> updated;
    for (const CommentData* cd : all) {
      if (!span.Contains(cd->span)) {
        updated.push_back(cd);
      }
    }
    all = updated;
  };

  std::vector<const AstNode*> under =
      CollectUnder(&node, /*want_types=*/false).value();
  for (const AstNode* descendant : under) {
    if (auto* e = dynamic_cast<const Expr*>(descendant);
        e != nullptr && e->IsBlockedExpr()) {
      remove_comments_under(e->span());
    }
  }

  return all;
}

DocRef Fmt(const BuiltinTypeAnnotation& n, const Comments& comments,
           DocArena& arena) {
  return arena.MakeText(BuiltinTypeToString(n.builtin_type()));
}

DocRef Fmt(const ArrayTypeAnnotation& n, const Comments& comments,
           DocArena& arena) {
  DocRef elem = Fmt(*n.element_type(), comments, arena);
  DocRef dim = Fmt(*n.dim(), comments, arena);
  return ConcatNGroup(arena, {elem, arena.obracket(), dim, arena.cbracket()});
}

static DocRef FmtTypeAnnotationPtr(const TypeAnnotation* n,
                                   const Comments& comments, DocArena& arena) {
  XLS_CHECK(n != nullptr);
  return Fmt(*n, comments, arena);
}

DocRef Fmt(const TupleTypeAnnotation& n, const Comments& comments,
           DocArena& arena) {
  std::vector<DocRef> pieces = {arena.oparen()};
  pieces.push_back(FmtJoin<const TypeAnnotation*>(
      n.members(), Joiner::kCommaSpace, FmtTypeAnnotationPtr, comments, arena));
  pieces.push_back(arena.cparen());
  return ConcatNGroup(arena, pieces);
}

DocRef Fmt(const TypeRef& n, const Comments& comments, DocArena& arena) {
  return absl::visit(
      Visitor{
          [&](const TypeAlias* n) { return arena.MakeText(n->identifier()); },
          [&](const StructDef* n) { return arena.MakeText(n->identifier()); },
          [&](const EnumDef* n) { return arena.MakeText(n->identifier()); },
          [&](const ColonRef* n) { return Fmt(*n, comments, arena); },
      },
      n.type_definition());
}

DocRef Fmt(const TypeRefTypeAnnotation& n, const Comments& comments,
           DocArena& arena) {
  std::vector<DocRef> pieces = {Fmt(*n.type_ref(), comments, arena)};
  if (!n.parametrics().empty()) {
    pieces.push_back(arena.oangle());
    pieces.push_back(FmtJoin<ExprOrType>(absl::MakeConstSpan(n.parametrics()),
                                         Joiner::kCommaSpace, FmtExprOrType,
                                         comments, arena));
    pieces.push_back(arena.cangle());
  }

  return ConcatNGroup(arena, pieces);
}

DocRef Fmt(const ChannelTypeAnnotation& n, const Comments& comments,
           DocArena& arena) {
  std::vector<DocRef> pieces = {
      arena.Make(Keyword::kChannel),
      arena.oangle(),
      Fmt(*n.payload(), comments, arena),
      arena.cangle(),
      arena.break1(),
      arena.Make(n.direction() == ChannelDirection::kIn ? Keyword::kIn
                                                        : Keyword::kOut),
  };
  if (n.dims().has_value()) {
    for (const Expr* dim : *n.dims()) {
      pieces.push_back(Fmt(*dim, comments, arena));
    }
  }
  return ConcatNGroup(arena, pieces);
}

DocRef Fmt(const TypeAnnotation& n, const Comments& comments, DocArena& arena) {
  if (auto* t = dynamic_cast<const BuiltinTypeAnnotation*>(&n)) {
    return Fmt(*t, comments, arena);
  }
  if (auto* t = dynamic_cast<const TupleTypeAnnotation*>(&n)) {
    return Fmt(*t, comments, arena);
  }
  if (auto* t = dynamic_cast<const ArrayTypeAnnotation*>(&n)) {
    return Fmt(*t, comments, arena);
  }
  if (auto* t = dynamic_cast<const TypeRefTypeAnnotation*>(&n)) {
    return Fmt(*t, comments, arena);
  }
  if (auto* t = dynamic_cast<const ChannelTypeAnnotation*>(&n)) {
    return Fmt(*t, comments, arena);
  }

  XLS_LOG(FATAL) << "handle type annotation: " << n.ToString()
                 << " type: " << n.GetNodeTypeName();
}

DocRef Fmt(const TypeAlias& n, const Comments& comments, DocArena& arena) {
  std::vector<DocRef> pieces;
  if (n.is_public()) {
    pieces.push_back(arena.Make(Keyword::kPub));
    pieces.push_back(arena.space());
  }
  pieces.push_back(arena.Make(Keyword::kType));
  pieces.push_back(arena.space());
  pieces.push_back(arena.MakeText(n.identifier()));
  pieces.push_back(arena.space());
  pieces.push_back(arena.equals());
  pieces.push_back(arena.break1());
  pieces.push_back(Fmt(*n.type_annotation(), comments, arena));
  return ConcatNGroup(arena, pieces);
}

DocRef Fmt(const NameDef& n, const Comments& comments, DocArena& arena) {
  return arena.MakeText(n.identifier());
}

DocRef Fmt(const NameRef& n, const Comments& comments, DocArena& arena) {
  // Check for special identifier for proc config, which is ProcName.config
  // internally, but in spawns we just want to say ProcName.
  if (auto pos = n.identifier().find('.'); pos != std::string::npos) {
    XLS_CHECK_EQ(n.identifier().substr(pos), ".config");
    return arena.MakeText(n.identifier().substr(0, pos));
  }
  return arena.MakeText(n.identifier());
}

DocRef Fmt(const Number& n, const Comments& comments, DocArena& arena) {
  DocRef num_text = arena.MakeText(n.text());
  if (const TypeAnnotation* type = n.type_annotation()) {
    return ConcatNGroup(arena, {Fmt(*type, comments, arena), arena.colon(),
                                arena.break0(), num_text});
  }
  return num_text;
}

DocRef Fmt(const WildcardPattern& n, const Comments& comments,
           DocArena& arena) {
  return arena.underscore();
}

DocRef Fmt(const Array& n, const Comments& comments, DocArena& arena) {
  std::vector<DocRef> leader_pieces;
  if (TypeAnnotation* t = n.type_annotation()) {
    leader_pieces.push_back(Fmt(*t, comments, arena));
    leader_pieces.push_back(arena.colon());
  }
  leader_pieces.push_back(arena.obracket());

  std::vector<DocRef> pieces;
  pieces.push_back(ConcatNGroup(arena, leader_pieces));
  pieces.push_back(arena.break0());

  std::vector<DocRef> member_pieces;
  member_pieces.push_back(FmtJoin<const Expr*>(
      n.members(), Joiner::kCommaBreak1AsGroup, FmtExprPtr, comments, arena));

  if (n.has_ellipsis()) {
    // Subtle implementation note: The Joiner::CommaBreak1AsGroup puts a
    // trailing comma when we're in break mode, so we only insert the comma for
    // ellipsis when we're in flat mode.
    member_pieces.push_back(arena.MakeFlatChoice(arena.comma(), arena.empty()));

    member_pieces.push_back(
        ConcatNGroup(arena, {arena.break1(), arena.MakeText("...")}));
  }

  pieces.push_back(arena.MakeNest(ConcatNGroup(arena, member_pieces)));
  pieces.push_back(arena.break0());
  pieces.push_back(arena.cbracket());

  return ConcatNGroup(arena, pieces);
}

DocRef Fmt(const Attr& n, const Comments& comments, DocArena& arena) {
  Precedence op_precedence = n.GetPrecedence();
  const Expr& lhs = *n.lhs();
  Precedence lhs_precedence = lhs.GetPrecedence();
  std::vector<DocRef> pieces;
  if (WeakerThan(lhs_precedence, op_precedence)) {
    pieces.push_back(arena.oparen());
    pieces.push_back(Fmt(lhs, comments, arena));
    pieces.push_back(arena.cparen());
  } else {
    pieces.push_back(Fmt(lhs, comments, arena));
  }
  pieces.push_back(arena.dot());
  pieces.push_back(arena.MakeText(std::string{n.attr()}));
  return ConcatNGroup(arena, pieces);
}

DocRef Fmt(const Binop& n, const Comments& comments, DocArena& arena) {
  Precedence op_precedence = n.GetPrecedence();
  const Expr& lhs = *n.lhs();
  const Expr& rhs = *n.rhs();
  Precedence lhs_precedence = lhs.GetPrecedence();

  auto emit = [&](const Expr& e, bool parens, std::vector<DocRef>& pieces) {
    if (parens) {
      pieces.push_back(arena.oparen());
      pieces.push_back(Fmt(e, comments, arena));
      pieces.push_back(arena.cparen());
    } else {
      pieces.push_back(Fmt(e, comments, arena));
    }
  };

  std::vector<DocRef> lhs_pieces;

  if (WeakerThan(lhs_precedence, op_precedence)) {
    // We have to parenthesize the LHS.
    emit(lhs, /*parens=*/true, lhs_pieces);
  } else if (n.binop_kind() == BinopKind::kLt &&
             lhs.kind() == AstNodeKind::kCast && !lhs.in_parens()) {
    // If there is an open angle bracket, and the LHS is suffixed with a type,
    // we parenthesize it to avoid ambiguity; e.g.
    //
    //    foo as bar < baz
    //           ^~~~~~~~^
    //
    // We don't know whether `bar<baz` is the start of a parametric type
    // instantiation, so we force conservative parenthesization:
    //
    //    (foo as bar) < baz
    emit(lhs, /*parens=*/true, lhs_pieces);
  } else {
    emit(lhs, /*parens=*/false, lhs_pieces);
  }

  lhs_pieces.push_back(arena.space());
  lhs_pieces.push_back(arena.MakeText(BinopKindFormat(n.binop_kind())));

  DocRef lhs_ref = ConcatNGroup(arena, lhs_pieces);

  std::vector<DocRef> rhs_pieces;
  if (WeakerThan(rhs.GetPrecedence(), op_precedence)) {
    emit(rhs, /*parens=*/true, rhs_pieces);
  } else {
    emit(rhs, /*parens=*/false, rhs_pieces);
  }

  std::vector<DocRef> top_pieces = {
      lhs_ref,
      arena.break1(),
      ConcatNGroup(arena, rhs_pieces),
  };

  return ConcatNGroup(arena, top_pieces);
}

// Note: if a comment doc is emitted (i.e. return value has_value()) it does not
// have a trailing hard-line. This is for consistency with other emission
// routines which generally don't emit any whitespace afterwards, just their
// doc.
static std::optional<DocRef> EmitCommentsBetween(
    std::optional<Pos> start_pos, const Pos& limit_pos,
    const Comments& comments, DocArena& arena,
    std::optional<Span>* last_comment_span) {
  if (!start_pos.has_value()) {
    start_pos = Pos(limit_pos.filename(), 0, 0);
  }
  XLS_CHECK_LE(start_pos.value(), limit_pos);
  const Span span(start_pos.value(), limit_pos);

  XLS_VLOG(3) << "Looking for comments in span: " << span;

  std::vector<DocRef> pieces;

  std::vector<const CommentData*> items = comments.GetComments(span);
  XLS_VLOG(3) << "Found " << items.size() << " comment data items";
  std::optional<Span> previous_comment_span;
  for (size_t i = 0; i < items.size(); ++i) {
    const CommentData* comment_data = items[i];

    // If the previous comment line and this comment line are abutted (i.e.
    // contiguous lines with comments), we don't put a newline between them.
    if (previous_comment_span.has_value() &&
        previous_comment_span->start().lineno() + 1 !=
            comment_data->span.start().lineno()) {
      XLS_VLOG(3) << "previous comment span: " << previous_comment_span.value()
                  << " this comment span: " << comment_data->span
                  << " -- inserting hard line";
      pieces.push_back(arena.hard_line());
    }

    pieces.push_back(arena.MakePrefixedReflow(
        "//",
        std::string{absl::StripTrailingAsciiWhitespace(comment_data->text)}));

    if (i + 1 != items.size()) {
      pieces.push_back(arena.hard_line());
    }

    previous_comment_span = comment_data->span;
    *last_comment_span = comment_data->span;
  }

  if (pieces.empty()) {
    return std::nullopt;
  }

  return ConcatN(arena, pieces);
}

// Note: we only add leading/trailing spaces in the block if add_curls is true.
static DocRef FmtBlock(const Block& n, const Comments& comments,
                       DocArena& arena, bool add_curls,
                       bool force_multiline = false) {
  bool has_comments = comments.HasComments(n.span());

  if (n.statements().empty() && !has_comments) {
    if (add_curls) {
      return ConcatNGroup(arena,
                          {arena.ocurl(), arena.break0(), arena.ccurl()});
    }
    return arena.break0();
  }

  // We only want to flatten single-statement blocks -- multi-statement blocks
  // we always make line breaks between the statements.
  if (n.statements().size() == 1 && !force_multiline && !has_comments) {
    std::vector<DocRef> pieces;
    if (add_curls) {
      pieces = {arena.ocurl(), arena.break1()};
    }

    pieces.push_back(Fmt(*n.statements()[0], comments, arena));

    if (n.trailing_semi()) {
      pieces.push_back(arena.semi());
    }
    if (add_curls) {
      pieces.push_back(arena.break1());
      pieces.push_back(arena.ccurl());
    }
    return arena.MakeNest(ConcatNGroup(arena, pieces));
  }

  // Emit a '{' then nest to emit statements with semis, then emit a '}' outside
  // the nesting.
  std::vector<DocRef> top;

  if (add_curls) {
    top.push_back(arena.ocurl());
    top.push_back(arena.hard_line());
  }

  Pos last_entity_pos = n.span().start();
  std::vector<DocRef> nested;
  for (size_t i = 0; i < n.statements().size(); ++i) {
    const Statement* stmt = n.statements()[i];

    // Get the start position for the statement.
    std::optional<Span> stmt_span = stmt->GetSpan();
    XLS_CHECK(stmt_span.has_value()) << stmt->ToString();
    Pos stmt_start = stmt_span->start();

    XLS_VLOG(5) << "stmt: `" << stmt->ToString() << "` start: " << stmt_start
                << " last_entity_pos: " << last_entity_pos;

    std::optional<Span> last_comment_span;
    if (std::optional<DocRef> comments_doc = EmitCommentsBetween(
            last_entity_pos, stmt_start, comments, arena, &last_comment_span)) {
      XLS_VLOG(5) << "last entity position: " << last_entity_pos
                  << " last_comment_span.start: " << last_comment_span->start();
      // If there's a line break between the last entity and this comment, we
      // retain it in the output (i.e. in paragraph style).
      if (last_entity_pos.lineno() + 1 < last_comment_span->start().lineno()) {
        nested.push_back(arena.hard_line());
      }

      nested.push_back(comments_doc.value());
      nested.push_back(arena.hard_line());

      last_entity_pos = last_comment_span->limit();
      XLS_VLOG(5) << "last comment position limit: " << last_entity_pos
                  << " comments_doc: "
                  << arena.Deref(comments_doc.value()).ToDebugString(arena);
    } else {  // No comments to emit ahead of the statement.
      // If there's a line break between the last entity and this statement, we
      // retain it in the output (i.e. in paragraph style).
      if (last_entity_pos.lineno() + 1 < stmt_span->start().lineno()) {
        nested.push_back(arena.hard_line());
      }

      last_entity_pos = stmt_span->limit();
    }

    // Here we emit the formatted statement.
    nested.push_back(Fmt(*stmt, comments, arena));
    bool last_stmt = i + 1 == n.statements().size();
    if (!last_stmt || n.trailing_semi()) {
      nested.push_back(arena.semi());
    }
    if (!last_stmt) {
      nested.push_back(arena.hard_line());
    }
  }

  // See if there are any comments to emit after the last statement to the end
  // of the block.
  std::optional<Span> last_comment_span;
  if (std::optional<DocRef> comments_doc =
          EmitCommentsBetween(last_entity_pos, n.span().limit(), comments,
                              arena, &last_comment_span)) {
    XLS_VLOG(5) << "last entity position: " << last_entity_pos
                << " last_comment_span.start: " << last_comment_span->start();

    // If there's a line break between the last entity and this comment, we
    // retain it in the output (i.e. in paragraph style).
    if (last_entity_pos.lineno() + 1 < last_comment_span->start().lineno()) {
      nested.push_back(arena.hard_line());
    }

    nested.push_back(arena.hard_line());
    nested.push_back(comments_doc.value());
  }

  top.push_back(arena.MakeNest(ConcatN(arena, nested)));
  if (add_curls) {
    top.push_back(arena.hard_line());
    top.push_back(arena.ccurl());
  }

  return ConcatNGroup(arena, top);
}

DocRef Fmt(const Block& n, const Comments& comments, DocArena& arena) {
  return FmtBlock(n, comments, arena, /*add_curls=*/true);
}

DocRef Fmt(const Cast& n, const Comments& comments, DocArena& arena) {
  DocRef lhs = Fmt(*n.expr(), comments, arena);

  Precedence arg_precedence = n.expr()->GetPrecedence();
  if (WeakerThan(arg_precedence, Precedence::kAs)) {
    lhs = ConcatN(arena, {arena.oparen(), lhs, arena.cparen()});
  }

  return ConcatNGroup(
      arena, {lhs, arena.space(), arena.Make(Keyword::kAs), arena.break1(),
              Fmt(*n.type_annotation(), comments, arena)});
}

DocRef Fmt(const ChannelDecl& n, const Comments& comments, DocArena& arena) {
  std::vector<DocRef> pieces{
      arena.Make(Keyword::kChannel),
      arena.oangle(),
      Fmt(*n.type(), comments, arena),
  };
  if (n.fifo_depth().has_value()) {
    pieces.push_back(arena.comma());
    pieces.push_back(arena.space());
    pieces.push_back(Fmt(*n.fifo_depth().value(), comments, arena));
  }
  pieces.push_back(arena.cangle());
  if (n.dims().has_value()) {
    for (const Expr* dim : *n.dims()) {
      pieces.push_back(Fmt(*dim, comments, arena));
    }
  }
  return ConcatNGroup(arena, pieces);
}

DocRef Fmt(const ColonRef& n, const Comments& comments, DocArena& arena) {
  DocRef subject = absl::visit(
      Visitor{[&](const NameRef* n) { return Fmt(*n, comments, arena); },
              [&](const ColonRef* n) { return Fmt(*n, comments, arena); }},
      n.subject());

  return ConcatNGroup(arena,
                      {subject, arena.colon_colon(), arena.MakeText(n.attr())});
}

DocRef Fmt(const For& n, const Comments& comments, DocArena& arena) {
  std::vector<DocRef> pieces = {
      arena.Make(Keyword::kFor),
      arena.space(),
      Fmt(*n.names(), comments, arena),
  };

  if (n.type_annotation() != nullptr) {
    pieces.push_back(arena.colon());
    pieces.push_back(arena.space());
    pieces.push_back(Fmt(*n.type_annotation(), comments, arena));
  }

  pieces.push_back(arena.space());
  pieces.push_back(arena.Make(Keyword::kIn));
  pieces.push_back(arena.space());
  pieces.push_back(Fmt(*n.iterable(), comments, arena));
  pieces.push_back(arena.space());
  pieces.push_back(arena.ocurl());

  std::vector<DocRef> body_pieces;
  body_pieces.push_back(arena.hard_line());
  body_pieces.push_back(FmtBlock(*n.body(), comments, arena,
                                 /*add_curls=*/false,
                                 /*force_multiline=*/true));
  body_pieces.push_back(arena.hard_line());
  body_pieces.push_back(arena.ccurl());
  body_pieces.push_back(arena.oparen());
  body_pieces.push_back(Fmt(*n.init(), comments, arena));
  body_pieces.push_back(arena.cparen());

  return arena.MakeConcat(ConcatNGroup(arena, pieces),
                          ConcatN(arena, body_pieces));
}

DocRef Fmt(const FormatMacro& n, const Comments& comments, DocArena& arena) {
  std::vector<DocRef> pieces = {
      arena.MakeText(n.macro()),
      arena.oparen(),
      arena.MakeText(
          absl::StrCat("\"", StepsToXlsFormatString(n.format()), "\"")),
      arena.comma(),
      arena.break1(),
  };
  pieces.push_back(FmtJoin<const Expr*>(n.args(), Joiner::kCommaSpace,
                                        FmtExprPtr, comments, arena));
  pieces.push_back(arena.cparen());
  return ConcatNGroup(arena, pieces);
}

DocRef Fmt(const Slice& n, const Comments& comments, DocArena& arena) {
  std::vector<DocRef> pieces;

  if (n.start() != nullptr) {
    pieces.push_back(Fmt(*n.start(), comments, arena));
  }
  pieces.push_back(arena.colon());
  if (n.limit() != nullptr) {
    pieces.push_back(Fmt(*n.limit(), comments, arena));
  }
  return ConcatNGroup(arena, pieces);
}

DocRef Fmt(const WidthSlice& n, const Comments& comments, DocArena& arena) {
  return ConcatNGroup(arena, {
                                 Fmt(*n.start(), comments, arena),
                                 arena.break0(),
                                 arena.plus_colon(),
                                 arena.break0(),
                                 Fmt(*n.width(), comments, arena),
                             });
}

static DocRef Fmt(const IndexRhs& n, const Comments& comments,
                  DocArena& arena) {
  return absl::visit(
      Visitor{
          [&](const Expr* n) { return Fmt(*n, comments, arena); },
          [&](const Slice* n) { return Fmt(*n, comments, arena); },
          [&](const WidthSlice* n) { return Fmt(*n, comments, arena); },
      },
      n);
}

DocRef Fmt(const Index& n, const Comments& comments, DocArena& arena) {
  std::vector<DocRef> pieces;
  if (WeakerThan(n.lhs()->GetPrecedence(), n.GetPrecedence())) {
    pieces.push_back(arena.oparen());
    pieces.push_back(Fmt(*n.lhs(), comments, arena));
    pieces.push_back(arena.cparen());
  } else {
    pieces.push_back(Fmt(*n.lhs(), comments, arena));
  }
  pieces.push_back(arena.obracket());
  pieces.push_back(Fmt(n.rhs(), comments, arena));
  pieces.push_back(arena.cbracket());
  return ConcatNGroup(arena, pieces);
}

DocRef FmtExprOrType(const ExprOrType& n, const Comments& comments,
                     DocArena& arena) {
  return absl::visit(
      Visitor{
          [&](const Expr* n) { return Fmt(*n, comments, arena); },
          [&](const TypeAnnotation* n) { return Fmt(*n, comments, arena); },
      },
      n);
}

DocRef Fmt(const Invocation& n, const Comments& comments, DocArena& arena) {
  std::vector<DocRef> pieces = {
      Fmt(*n.callee(), comments, arena),
  };
  if (!n.explicit_parametrics().empty()) {
    pieces.push_back(arena.oangle());
    pieces.push_back(FmtJoin<ExprOrType>(
        absl::MakeConstSpan(n.explicit_parametrics()), Joiner::kCommaSpace,
        FmtExprOrType, comments, arena));
    pieces.push_back(arena.cangle());
  }
  pieces.push_back(arena.oparen());
  pieces.push_back(FmtJoin<const Expr*>(n.args(), Joiner::kCommaSpace,
                                        FmtExprPtr, comments, arena));
  pieces.push_back(arena.cparen());
  return ConcatNGroup(arena, pieces);
}

static DocRef FmtNameDefTreePtr(const NameDefTree* n, const Comments& comments,
                                DocArena& arena) {
  return Fmt(*n, comments, arena);
}

static DocRef Fmt(const MatchArm& n, const Comments& comments,
                  DocArena& arena) {
  std::vector<DocRef> pieces;
  pieces.push_back(
      FmtJoin<const NameDefTree*>(n.patterns(), Joiner::kSpaceBarBreak,
                                  FmtNameDefTreePtr, comments, arena));
  pieces.push_back(arena.space());
  pieces.push_back(arena.fat_arrow());
  pieces.push_back(arena.break1());
  pieces.push_back(Fmt(*n.expr(), comments, arena));
  return ConcatNGroup(arena, pieces);
}

DocRef Fmt(const Match& n, const Comments& comments, DocArena& arena) {
  std::vector<DocRef> pieces;
  pieces.push_back(ConcatNGroup(
      arena,
      {arena.Make(Keyword::kMatch), arena.space(),
       Fmt(n.matched(), comments, arena), arena.space(), arena.ocurl()}));

  pieces.push_back(arena.hard_line());

  for (const MatchArm* arm : n.arms()) {
    pieces.push_back(arena.MakeNest(Fmt(*arm, comments, arena)));
    pieces.push_back(arena.comma());
    pieces.push_back(arena.hard_line());
  }

  pieces.push_back(arena.ccurl());
  return ConcatN(arena, pieces);
}

DocRef Fmt(const Spawn& n, const Comments& comments, DocArena& arena) {
  return ConcatNGroup(arena, {arena.Make(Keyword::kSpawn), arena.space(),
                              Fmt(*n.config(), comments, arena)}

  );
}

DocRef Fmt(const XlsTuple& n, const Comments& comments, DocArena& arena) {
  std::vector<DocRef> pieces;

  // 1-element tuples are a special case- we always want a trailing comma and
  // never want it to be broken up. Handle separately here.
  if (n.members().size() == 1) {
    return ConcatNGroup(arena, {
                                   arena.oparen(),
                                   Fmt(*n.members()[0], comments, arena),
                                   arena.comma(),
                                   arena.cparen(),
                               });
  }

  for (size_t i = 0; i < n.members().size(); ++i) {
    bool last_element = i + 1 == n.members().size();
    const Expr* member = n.members()[i];
    DocRef member_doc = Fmt(*member, comments, arena);
    if (last_element) {
      pieces.push_back(arena.MakeGroup(member_doc));
      pieces.push_back(arena.MakeFlatChoice(
          /*on_flat=*/arena.empty(),
          /*on_break=*/arena.comma()));
    } else {
      pieces.push_back(ConcatNGroup(arena, {
                                               arena.MakeGroup(member_doc),
                                               arena.comma(),
                                               arena.break1(),
                                           }));
    }
  }

  return ConcatNGroup(
      arena, {
                 arena.oparen(),
                 arena.MakeFlatChoice(
                     /*on_flat=*/ConcatNGroup(arena, pieces),
                     /*on_break=*/ConcatNGroup(
                         arena,
                         {
                             arena.hard_line(),
                             arena.MakeNest(ConcatNGroup(arena, pieces)),
                             arena.hard_line(),
                         })),
                 arena.cparen(),
             });
}

static DocRef Fmt(const StructRef& n, const Comments& comments,
                  DocArena& arena) {
  return absl::visit(
      Visitor{
          [&](const StructDef* n) { return arena.MakeText(n->identifier()); },
          [&](const ColonRef* n) { return Fmt(*n, comments, arena); },
      },
      n);
}

// Note: this does not put any spacing characters after the '{' so we can
// appropriately handle the case of an empty struct having no spacing in its
// `S {}` style construct.
static DocRef FmtStructLeader(const StructRef& struct_ref,
                              const Comments& comments, DocArena& arena) {
  return ConcatNGroup(arena, {
                                 Fmt(struct_ref, comments, arena),
                                 arena.break1(),
                                 arena.ocurl(),
                             });
}

static DocRef FmtStructMembers(
    absl::Span<const std::pair<std::string, Expr*>> members,
    const Comments& comments, DocArena& arena) {
  return FmtJoin<std::pair<std::string, Expr*>>(
      members, Joiner::kCommaBreak1,
      [](const auto& member, const Comments& comments, DocArena& arena) {
        const auto& [name, expr] = member;
        // If the expression is an identifier that matches its corresponding
        // struct member name, we canonically use the shorthand notation of just
        // providing the identifier and leaving the member name implicitly as
        // the same symbol.
        if (const NameRef* name_ref = dynamic_cast<const NameRef*>(expr);
            name_ref != nullptr && name_ref->identifier() == name) {
          return arena.MakeText(name);
        }

        return ConcatNGroup(
            arena, {arena.MakeText(name), arena.colon(), arena.break1(),
                    Fmt(*expr, comments, arena)});
      },
      comments, arena);
}

DocRef Fmt(const StructInstance& n, const Comments& comments, DocArena& arena) {
  DocRef leader = FmtStructLeader(n.struct_def(), comments, arena);

  if (n.GetUnorderedMembers().empty()) {  // empty struct instance
    return arena.MakeConcat(leader, arena.ccurl());
  }

  // Implementation note: we cannot reorder members to be canonically the same
  // order as the struct definition in the general case, since the struct
  // definition may be defined an an imported file, and we have auto-formatting
  // work purely at the single-file syntax level.
  DocRef body_pieces =
      FmtStructMembers(n.GetUnorderedMembers(), comments, arena);

  return ConcatNGroup(arena,
                      {leader, arena.break1(), arena.MakeNest(body_pieces),
                       arena.break1(), arena.ccurl()});
}

DocRef Fmt(const SplatStructInstance& n, const Comments& comments,
           DocArena& arena) {
  DocRef leader = FmtStructLeader(n.struct_ref(), comments, arena);
  if (n.members().empty()) {
    return ConcatNGroup(arena, {leader, arena.break1(), arena.dot_dot(),
                                Fmt(*n.splatted(), comments, arena),
                                arena.break1(), arena.ccurl()});
  }

  DocRef body_pieces = FmtStructMembers(n.members(), comments, arena);

  return ConcatNGroup(
      arena,
      {leader, arena.break1(), arena.MakeNest(body_pieces), arena.comma(),
       arena.break1(), arena.dot_dot(), Fmt(*n.splatted(), comments, arena),
       arena.break1(), arena.ccurl()});

  XLS_LOG(FATAL) << "handle splat struct instance: " << n.ToString();
}

DocRef Fmt(const String& n, const Comments& comments, DocArena& arena) {
  return arena.MakeText(n.ToString());
}

// Creates a group that has the "test portion" of the conditional; i.e.
//
//  if <break1> $test_expr <break1> {
static DocRef MakeConditionalTestGroup(const Conditional& n,
                                       const Comments& comments,
                                       DocArena& arena) {
  return ConcatNGroup(arena, {
                                 arena.Make(Keyword::kIf),
                                 arena.break1(),
                                 Fmt(*n.test(), comments, arena),
                                 arena.break1(),
                                 arena.ocurl(),
                             });
}

// When there's an else-if, or multiple statements inside of the blocks, we
// force the formatting to be multi-line.
static DocRef FmtConditionalMultiline(const Conditional& n,
                                      const Comments& comments,
                                      DocArena& arena) {
  std::vector<DocRef> pieces = {
      MakeConditionalTestGroup(n, comments, arena), arena.hard_line(),
      FmtBlock(*n.consequent(), comments, arena, /*add_curls=*/false),
      arena.hard_line()};

  std::variant<Block*, Conditional*> alternate = n.alternate();
  while (std::holds_alternative<Conditional*>(alternate)) {
    Conditional* elseif = std::get<Conditional*>(alternate);
    alternate = elseif->alternate();
    pieces.push_back(arena.ccurl());
    pieces.push_back(arena.space());
    pieces.push_back(arena.Make(Keyword::kElse));
    pieces.push_back(arena.space());
    pieces.push_back(MakeConditionalTestGroup(*elseif, comments, arena));
    pieces.push_back(arena.hard_line());
    pieces.push_back(
        FmtBlock(*elseif->consequent(), comments, arena, /*add_curls=*/false));
    pieces.push_back(arena.hard_line());
  }

  XLS_CHECK(std::holds_alternative<Block*>(alternate));

  Block* else_block = std::get<Block*>(alternate);
  pieces.push_back(arena.ccurl());
  pieces.push_back(arena.space());
  pieces.push_back(arena.Make(Keyword::kElse));
  pieces.push_back(arena.space());
  pieces.push_back(arena.ocurl());
  pieces.push_back(arena.hard_line());
  pieces.push_back(FmtBlock(*else_block, comments, arena, /*add_curls=*/false));
  pieces.push_back(arena.hard_line());
  pieces.push_back(arena.ccurl());

  return ConcatN(arena, pieces);
}

DocRef Fmt(const Conditional& n, const Comments& comments, DocArena& arena) {
  // If there's an else-if clause or multi-statement blocks we force it to be
  // multi-line.
  if (n.HasElseIf() || n.HasMultiStatementBlocks()) {
    return FmtConditionalMultiline(n, comments, arena);
  }

  std::vector<DocRef> pieces = {
      MakeConditionalTestGroup(n, comments, arena),
      arena.break1(),
      FmtBlock(*n.consequent(), comments, arena, /*add_curls=*/false),
      arena.break1(),
  };

  XLS_CHECK(std::holds_alternative<Block*>(n.alternate()));
  const Block* else_block = std::get<Block*>(n.alternate());
  pieces.push_back(ConcatNGroup(
      arena, {arena.ccurl(), arena.break1(), arena.Make(Keyword::kElse),
              arena.break1(), arena.ocurl(), arena.break1()}));
  pieces.push_back(FmtBlock(*else_block, comments, arena, /*add_curls=*/false));
  pieces.push_back(arena.break1());
  pieces.push_back(arena.ccurl());
  return ConcatNGroup(arena, pieces);
}

DocRef Fmt(const ConstAssert& n, const Comments& comments, DocArena& arena) {
  return ConcatNGroup(arena, {
                                 arena.MakeText("const_assert!("),
                                 Fmt(*n.arg(), comments, arena),
                                 arena.cparen(),
                             });
}

DocRef Fmt(const TupleIndex& n, const Comments& comments, DocArena& arena) {
  std::vector<DocRef> pieces;
  if (WeakerThan(n.lhs()->GetPrecedence(), n.GetPrecedence())) {
    pieces.push_back(arena.oparen());
    pieces.push_back(Fmt(*n.lhs(), comments, arena));
    pieces.push_back(arena.cparen());
  } else {
    pieces.push_back(Fmt(*n.lhs(), comments, arena));
  }

  pieces.push_back(arena.dot());
  pieces.push_back(Fmt(*n.index(), comments, arena));
  return ConcatNGroup(arena, pieces);
}

DocRef Fmt(const UnrollFor& n, const Comments& comments, DocArena& arena) {
  XLS_LOG(FATAL) << "handle unroll for: " << n.ToString();
}

DocRef Fmt(const ZeroMacro& n, const Comments& comments, DocArena& arena) {
  return ConcatNGroup(arena, {
                                 arena.MakeText("zero!"),
                                 arena.oangle(),
                                 FmtExprOrType(n.type(), comments, arena),
                                 arena.cangle(),
                                 arena.oparen(),
                                 arena.cparen(),
                             });
}

DocRef Fmt(const Unop& n, const Comments& comments, DocArena& arena) {
  std::vector<DocRef> pieces = {
      arena.MakeText(UnopKindToString(n.unop_kind()))};
  if (WeakerThan(n.operand()->GetPrecedence(), n.GetPrecedence())) {
    pieces.push_back(arena.oparen());
    pieces.push_back(Fmt(*n.operand(), comments, arena));
    pieces.push_back(arena.cparen());
  } else {
    pieces.push_back(Fmt(n.operand(), comments, arena));
  }
  return ConcatNGroup(arena, pieces);
}

// Forward decl.
DocRef Fmt(const Range& n, const Comments& comments, DocArena& arena);
DocRef Fmt(const Let& n, const Comments& comments, DocArena& arena);

class FmtExprVisitor : public ExprVisitor {
 public:
  FmtExprVisitor(DocArena& arena, const Comments& comments)
      : arena_(arena), comments_(comments) {}

  ~FmtExprVisitor() override = default;

#define DEFINE_HANDLER(__type)                               \
  absl::Status Handle##__type(const __type* expr) override { \
    result_ = Fmt(*expr, comments_, arena_);                 \
    return absl::OkStatus();                                 \
  }

  XLS_DSLX_EXPR_NODE_EACH(DEFINE_HANDLER)

#undef DEFINE_HANDLER

  DocRef result() const { return result_.value(); }

 private:
  DocArena& arena_;
  const Comments& comments_;
  std::optional<DocRef> result_;
};

DocRef Fmt(const Range& n, const Comments& comments, DocArena& arena) {
  return ConcatNGroup(
      arena, {Fmt(*n.start(), comments, arena), arena.break0(), arena.dot_dot(),
              arena.break0(), Fmt(*n.end(), comments, arena)});
}

DocRef Fmt(const NameDefTree::Leaf& n, const Comments& comments,
           DocArena& arena) {
  return absl::visit(
      Visitor{
          [&](const NameDef* n) { return Fmt(*n, comments, arena); },
          [&](const NameRef* n) { return Fmt(*n, comments, arena); },
          [&](const WildcardPattern* n) { return Fmt(*n, comments, arena); },
          [&](const Number* n) { return Fmt(*n, comments, arena); },
          [&](const ColonRef* n) { return Fmt(*n, comments, arena); },
          [&](const Range* n) { return Fmt(*n, comments, arena); },
      },
      n);
}

DocRef Fmt(const NameDefTree& n, const Comments& comments, DocArena& arena) {
  if (n.is_leaf()) {
    return Fmt(n.leaf(), comments, arena);
  }
  std::vector<DocRef> pieces = {arena.oparen()};
  std::vector<std::variant<NameDefTree::Leaf, NameDefTree*>> flattened =
      n.Flatten1();
  for (size_t i = 0; i < flattened.size(); ++i) {
    const auto& item = flattened[i];
    absl::visit(Visitor{
                    [&](const NameDefTree::Leaf& leaf) {
                      pieces.push_back(Fmt(leaf, comments, arena));
                    },
                    [&](const NameDefTree* subtree) {
                      pieces.push_back(Fmt(*subtree, comments, arena));
                    },
                },
                item);
    if (i + 1 != flattened.size()) {
      pieces.push_back(arena.comma());
      pieces.push_back(arena.break1());
    }
  }
  pieces.push_back(arena.cparen());
  return ConcatNGroup(arena, pieces);
}

DocRef Fmt(const Let& n, const Comments& comments, DocArena& arena) {
  DocRef break1 = arena.break1();

  std::vector<DocRef> leader_pieces = {
      arena.MakeText(n.is_const() ? "const" : "let"), break1,
      Fmt(*n.name_def_tree(), comments, arena)};
  if (const TypeAnnotation* t = n.type_annotation()) {
    leader_pieces.push_back(arena.colon());
    leader_pieces.push_back(break1);
    leader_pieces.push_back(Fmt(*t, comments, arena));
  }

  leader_pieces.push_back(break1);
  leader_pieces.push_back(arena.equals());
  leader_pieces.push_back(break1);

  DocRef leader = ConcatNGroup(arena, leader_pieces);
  DocRef body;
  if (n.rhs()->IsBlockedExpr() || n.rhs()->kind() == AstNodeKind::kArray) {
    // For blocked expressions we don't align them to the equals in the let,
    // because it'd shove constructs like `let really_long_identifier = for ...`
    // too far to the right hand side.
    //
    // Similarly for array literals, as they can have lots of elements which
    // effectively makes them like blocks.
    //
    // Note that if you do e.g. a binary operation on blocked constructs as the
    // RHS it /will/ align because we don't look for blocked constructs
    // transitively -- seems reasonable given that's going to look funky no
    // matter what.
    body = Fmt(*n.rhs(), comments, arena);
  } else {
    body = arena.MakeAlign(Fmt(*n.rhs(), comments, arena));
  }

  DocRef syntax = arena.MakeConcat(leader, body);

  std::vector<const CommentData*> comment_data =
      GetCommentsForNode(n, n.span(), comments);
  if (comment_data.size() == 1) {
    std::string comment_text = comment_data[0]->text;
    if (!comment_text.empty() && comment_text.back() == '\n') {
      comment_text.pop_back();
    }

    DocRef comment_text_ref = arena.MakeText(comment_text);

    // If it's a single line comment we create a FlatChoice between:
    //    let ... // comment text
    //
    // and:
    //
    //    // comment text reflowed with // prefix
    //    let ...
    DocRef flat = ConcatN(
        arena, {syntax, arena.space(), arena.slash_slash(), comment_text_ref});

    // TODO(leary): 2023-09-30 Make this so it reflows overlong lines in the
    // comment text with the // prefix inserted at the indentation level.
    DocRef line_prefixed = ConcatN(
        arena,
        {arena.slash_slash(), comment_text_ref, arena.hard_line(), syntax});
    return arena.MakeGroup(arena.MakeFlatChoice(flat, line_prefixed));
  }

  if (!comment_data.empty()) {
    XLS_LOG(FATAL) << "let: multiple inline comments @ "
                   << absl::StrJoin(
                          comment_data, ", ",
                          [](std::string* out, const CommentData* data) {
                            absl::StrAppend(out, data->span.ToString());
                          });
  }

  return syntax;
}

}  // namespace

/* static */ Comments Comments::Create(absl::Span<const CommentData> comments) {
  std::optional<Pos> last_data_limit;
  absl::flat_hash_map<int64_t, CommentData> line_to_comment;
  for (const CommentData& cd : comments) {
    XLS_VLOG(3) << "comment on line: " << cd.span.start().lineno();
    // Note: we don't have multi-line comments for now, so we just note the
    // start line number for the comment.
    line_to_comment[cd.span.start().lineno()] = cd;
    if (last_data_limit.has_value()) {
      last_data_limit = std::max(cd.span.limit(), last_data_limit.value());
    } else {
      last_data_limit = cd.span.limit();
    }
  }
  return Comments{std::move(line_to_comment), last_data_limit};
}

bool Comments::HasComments(const Span& in_span) const {
  for (int64_t i = in_span.start().lineno(); i <= in_span.limit().lineno();
       ++i) {
    if (auto it = line_to_comment_.find(i); it != line_to_comment_.end()) {
      return true;
    }
  }
  return false;
}

std::vector<const CommentData*> Comments::GetComments(
    const Span& node_span) const {
  XLS_VLOG(3) << "GetComments; node_span: " << node_span;

  // Implementation note: this will typically be a single access (as most things
  // will be on a single line), so we prefer a flat hash map to a btree map.
  std::vector<const CommentData*> results;
  for (int64_t i = node_span.start().lineno(); i <= node_span.limit().lineno();
       ++i) {
    if (auto it = line_to_comment_.find(i); it != line_to_comment_.end()) {
      results.push_back(&it->second);
    }
  }
  return results;
}

DocRef Fmt(const Statement& n, const Comments& comments, DocArena& arena) {
  return absl::visit(
      Visitor{
          [&](const Expr* n) { return Fmt(*n, comments, arena); },
          [&](const TypeAlias* n) { return Fmt(*n, comments, arena); },
          [&](const Let* n) { return Fmt(*n, comments, arena); },
          [&](const ConstAssert* n) { return Fmt(*n, comments, arena); },
      },
      n.wrapped());
}

// Formats parameters (i.e. function parameters) with leading '(' and trailing
// ')'.
static DocRef FmtParams(absl::Span<const Param* const> params,
                        const Comments& comments, DocArena& arena) {
  std::vector<DocRef> pieces = {arena.oparen()};
  for (size_t i = 0; i < params.size(); ++i) {
    const Param* param = params[i];
    DocRef type = Fmt(*param->type_annotation(), comments, arena);
    std::vector<DocRef> param_pieces = {arena.MakeText(param->identifier()),
                                        arena.break0(), arena.colon(),
                                        arena.break1(), type};
    if (i + 1 != params.size()) {
      param_pieces.push_back(arena.comma());
      param_pieces.push_back(arena.break1());
    }
    pieces.push_back(ConcatNGroup(arena, param_pieces));
  }
  pieces.push_back(arena.cparen());
  return ConcatNGroup(arena, pieces);
}

static DocRef Fmt(const ParametricBinding& n, const Comments& comments,
                  DocArena& arena) {
  std::vector<DocRef> pieces = {
      arena.MakeText(n.identifier()),
      arena.colon(),
      arena.break1(),
      Fmt(*n.type_annotation(), comments, arena),
  };
  if (n.expr() != nullptr) {
    pieces.push_back(arena.space());
    pieces.push_back(arena.equals());
    pieces.push_back(arena.space());
    pieces.push_back(arena.ocurl());
    pieces.push_back(arena.break0());
    pieces.push_back(arena.MakeNest(Fmt(*n.expr(), comments, arena)));
    pieces.push_back(arena.ccurl());
  }
  return ConcatNGroup(arena, pieces);
}

static DocRef FmtParametricBindingPtr(const ParametricBinding* n,
                                      const Comments& comments,
                                      DocArena& arena) {
  XLS_CHECK(n != nullptr);
  return Fmt(*n, comments, arena);
}

DocRef Fmt(const Function& n, const Comments& comments, DocArena& arena) {
  std::vector<DocRef> signature_pieces;
  if (n.is_public()) {
    signature_pieces.push_back(arena.Make(Keyword::kPub));
    signature_pieces.push_back(arena.space());
  }
  signature_pieces.push_back(arena.Make(Keyword::kFn));
  signature_pieces.push_back(arena.space());
  signature_pieces.push_back(arena.MakeText(n.identifier()));

  if (n.IsParametric()) {
    signature_pieces.push_back(
        ConcatNGroup(arena, {arena.oangle(),
                             FmtJoin<const ParametricBinding*>(
                                 n.parametric_bindings(), Joiner::kCommaSpace,
                                 FmtParametricBindingPtr, comments, arena),
                             arena.cangle()}));
  }

  {
    std::vector<DocRef> params_pieces;

    params_pieces.push_back(arena.break0());
    params_pieces.push_back(FmtParams(n.params(), comments, arena));

    if (n.return_type() == nullptr) {
      params_pieces.push_back(arena.break1());
      params_pieces.push_back(arena.ocurl());
    } else {
      params_pieces.push_back(
          ConcatNGroup(arena, {
                                  arena.break1(),
                                  arena.arrow(),
                                  arena.break1(),
                                  Fmt(*n.return_type(), comments, arena),
                                  arena.break1(),
                                  arena.ocurl(),
                              }));
    }

    signature_pieces.push_back(
        arena.MakeNest(ConcatNGroup(arena, params_pieces)));
  }

  // For empty function we don't put spaces between the curls.
  if (n.body()->empty()) {
    std::vector<DocRef> fn_pieces = {
        ConcatNGroup(arena, signature_pieces),
        FmtBlock(*n.body(), comments, arena, /*add_curls=*/false),
        arena.ccurl(),
    };

    return ConcatNGroup(arena, fn_pieces);
  }

  std::vector<DocRef> fn_pieces = {
      ConcatNGroup(arena, signature_pieces),
      arena.break1(),
      FmtBlock(*n.body(), comments, arena, /*add_curls=*/false),
      arena.break1(),
      arena.ccurl(),
  };

  return ConcatNGroup(arena, fn_pieces);
}
static DocRef Fmt(const ProcMember& n, const Comments& comments,
                  DocArena& arena) {
  return ConcatNGroup(
      arena, {Fmt(*n.name_def(), comments, arena), arena.colon(),
              arena.break1(), Fmt(*n.type_annotation(), comments, arena)});
}

static DocRef Fmt(const Proc& n, const Comments& comments, DocArena& arena) {
  std::vector<DocRef> signature_pieces;
  if (n.is_public()) {
    signature_pieces.push_back(arena.Make(Keyword::kPub));
    signature_pieces.push_back(arena.space());
  }
  signature_pieces.push_back(arena.Make(Keyword::kProc));
  signature_pieces.push_back(arena.space());
  signature_pieces.push_back(arena.MakeText(n.identifier()));

  if (n.IsParametric()) {
    signature_pieces.push_back(
        ConcatNGroup(arena, {arena.oangle(),
                             FmtJoin<const ParametricBinding*>(
                                 n.parametric_bindings(), Joiner::kCommaSpace,
                                 FmtParametricBindingPtr, comments, arena),
                             arena.cangle()}));
  }
  signature_pieces.push_back(arena.break1());
  signature_pieces.push_back(arena.ocurl());

  std::vector<DocRef> member_pieces;
  member_pieces.reserve(n.members().size());
  for (const ProcMember* member : n.members()) {
    member_pieces.push_back(Fmt(*member, comments, arena));
    member_pieces.push_back(arena.semi());
    member_pieces.push_back(arena.hard_line());
  }

  std::vector<DocRef> config_pieces = {
      arena.MakeText("config"),
      FmtParams(n.config()->params(), comments, arena),
      arena.space(),
      arena.ocurl(),
      arena.break1(),
      FmtBlock(*n.config()->body(), comments, arena, /*add_curls=*/false),
      arena.break1(),
      arena.ccurl(),
  };

  std::vector<DocRef> init_pieces = {
      arena.MakeText("init"),
      arena.space(),
      arena.ocurl(),
      arena.break1(),
      FmtBlock(*n.init()->body(), comments, arena, /*add_curls=*/false),
      arena.break1(),
      arena.ccurl(),
  };

  std::vector<DocRef> next_pieces = {
      arena.MakeText("next"),
      FmtParams(n.next()->params(), comments, arena),
      arena.space(),
      arena.ocurl(),
      arena.break1(),
      FmtBlock(*n.next()->body(), comments, arena, /*add_curls=*/false),
      arena.break1(),
      arena.ccurl(),
  };

  std::vector<DocRef> proc_pieces = {
      ConcatNGroup(arena, signature_pieces),
      arena.hard_line(),
      member_pieces.empty()
          ? arena.empty()
          : ConcatNGroup(arena,
                         {
                             arena.MakeNest(ConcatNGroup(arena, member_pieces)),
                             arena.hard_line(),
                         }),
      arena.MakeNest(ConcatNGroup(arena, config_pieces)),
      arena.hard_line(),
      arena.hard_line(),
      arena.MakeNest(ConcatNGroup(arena, init_pieces)),
      arena.hard_line(),
      arena.hard_line(),
      arena.MakeNest(ConcatNGroup(arena, next_pieces)),
      arena.hard_line(),
      arena.ccurl(),
  };

  return ConcatNGroup(arena, proc_pieces);
}

static DocRef Fmt(const TestFunction& n, const Comments& comments,
                  DocArena& arena) {
  std::vector<DocRef> pieces;
  pieces.push_back(arena.MakeText("#[test]"));
  pieces.push_back(arena.hard_line());
  pieces.push_back(Fmt(*n.fn(), comments, arena));
  return ConcatN(arena, pieces);
}

static DocRef Fmt(const TestProc& n, const Comments& comments,
                  DocArena& arena) {
  std::vector<DocRef> pieces;
  pieces.push_back(arena.MakeText("#[test_proc]"));
  pieces.push_back(arena.hard_line());
  pieces.push_back(Fmt(*n.proc(), comments, arena));
  return ConcatN(arena, pieces);
}

static DocRef Fmt(const QuickCheck& n, const Comments& comments,
                  DocArena& arena) {
  std::vector<DocRef> pieces;
  pieces.push_back(arena.MakeText("#[quickcheck]"));
  pieces.push_back(arena.hard_line());
  pieces.push_back(Fmt(*n.f(), comments, arena));
  return ConcatN(arena, pieces);
}

static DocRef Fmt(const StructDef& n, const Comments& comments,
                  DocArena& arena) {
  std::vector<DocRef> pieces;
  if (n.is_public()) {
    pieces.push_back(arena.Make(Keyword::kPub));
    pieces.push_back(arena.space());
  }
  pieces.push_back(arena.Make(Keyword::kStruct));
  pieces.push_back(arena.space());
  pieces.push_back(arena.MakeText(n.identifier()));

  if (!n.parametric_bindings().empty()) {
    pieces.push_back(arena.oangle());
    pieces.push_back(FmtJoin<const ParametricBinding*>(
        n.parametric_bindings(), Joiner::kCommaSpace, FmtParametricBindingPtr,
        comments, arena));
    pieces.push_back(arena.cangle());
  }

  pieces.push_back(arena.space());
  pieces.push_back(arena.ocurl());

  if (!n.members().empty()) {
    pieces.push_back(arena.break1());

    std::vector<DocRef> body_pieces;
    for (size_t i = 0; i < n.members().size(); ++i) {
      const auto& [name_def, type] = n.members()[i];
      body_pieces.push_back(arena.MakeText(name_def->identifier()));
      body_pieces.push_back(arena.colon());
      body_pieces.push_back(arena.space());
      body_pieces.push_back(Fmt(*type, comments, arena));
      if (i + 1 == n.members().size()) {
        body_pieces.push_back(arena.MakeFlatChoice(/*on_flat=*/arena.empty(),
                                                   /*on_break=*/arena.comma()));
      } else {
        body_pieces.push_back(arena.comma());
        body_pieces.push_back(arena.break1());
      }
    }

    pieces.push_back(arena.MakeNest(ConcatN(arena, body_pieces)));
    pieces.push_back(arena.break1());
  }

  pieces.push_back(arena.ccurl());
  return ConcatNGroup(arena, pieces);
}

static DocRef Fmt(const ConstantDef& n, const Comments& comments,
                  DocArena& arena) {
  std::vector<DocRef> leader_pieces;
  if (n.is_public()) {
    leader_pieces.push_back(arena.Make(Keyword::kPub));
    leader_pieces.push_back(arena.break1());
  }
  leader_pieces.push_back(arena.Make(Keyword::kConst));
  leader_pieces.push_back(arena.break1());
  leader_pieces.push_back(arena.MakeText(n.identifier()));
  leader_pieces.push_back(arena.break1());
  leader_pieces.push_back(arena.equals());
  leader_pieces.push_back(arena.space());

  std::vector<DocRef> pieces;
  pieces.push_back(ConcatNGroup(arena, leader_pieces));
  pieces.push_back(Fmt(*n.value(), comments, arena));
  pieces.push_back(arena.semi());
  return ConcatNGroup(arena, pieces);
}

static DocRef FmtEnumMember(const EnumMember& n, const Comments& comments,
                            DocArena& arena) {
  return ConcatNGroup(
      arena, {Fmt(*n.name_def, comments, arena), arena.space(), arena.equals(),
              arena.break1(), Fmt(*n.value, comments, arena), arena.comma()});
}

static DocRef Fmt(const EnumDef& n, const Comments& comments, DocArena& arena) {
  std::vector<DocRef> pieces;
  if (n.is_public()) {
    pieces.push_back(arena.Make(Keyword::kPub));
    pieces.push_back(arena.space());
  }
  pieces.push_back(arena.Make(Keyword::kEnum));
  pieces.push_back(arena.space());
  pieces.push_back(arena.MakeText(n.identifier()));

  pieces.push_back(arena.space());
  if (n.type_annotation() != nullptr) {
    pieces.push_back(arena.colon());
    pieces.push_back(arena.space());
    pieces.push_back(Fmt(*n.type_annotation(), comments, arena));
    pieces.push_back(arena.space());
  }

  pieces.push_back(arena.ocurl());
  pieces.push_back(arena.hard_line());

  DocRef nested = FmtJoin<EnumMember>(n.values(), Joiner::kHardLine,
                                      FmtEnumMember, comments, arena);

  pieces.push_back(arena.MakeNest(nested));
  pieces.push_back(arena.hard_line());
  pieces.push_back(arena.ccurl());
  return ConcatN(arena, pieces);
}

static DocRef Fmt(const Import& n, const Comments& comments, DocArena& arena) {
  std::vector<DocRef> dotted_pieces;
  for (size_t i = 0; i < n.subject().size(); ++i) {
    const std::string& subject_part = n.subject()[i];
    DocRef this_doc_ref;
    if (i + 1 == n.subject().size()) {
      this_doc_ref = ConcatNGroup(arena, {arena.MakeText(subject_part)});
    } else {
      this_doc_ref = ConcatNGroup(
          arena, {arena.MakeText(subject_part), arena.dot(), arena.break0()});
    }
    dotted_pieces.push_back(this_doc_ref);
  }

  std::vector<DocRef> pieces = {
      arena.Make(Keyword::kImport), arena.space(),
      arena.MakeAlign(ConcatNGroup(arena, dotted_pieces))};

  if (const std::optional<std::string>& alias = n.alias()) {
    pieces.push_back(arena.break1());
    pieces.push_back(arena.Make(Keyword::kAs));
    pieces.push_back(arena.break1());
    pieces.push_back(arena.MakeText(alias.value()));
  }

  return ConcatNGroup(arena, pieces);
}

static DocRef Fmt(const ModuleMember& n, const Comments& comments,
                  DocArena& arena) {
  return absl::visit(
      Visitor{[&](const Function* n) { return Fmt(*n, comments, arena); },
              [&](const Proc* n) { return Fmt(*n, comments, arena); },
              [&](const TestFunction* n) { return Fmt(*n, comments, arena); },
              [&](const TestProc* n) { return Fmt(*n, comments, arena); },
              [&](const QuickCheck* n) { return Fmt(*n, comments, arena); },
              [&](const TypeAlias* n) {
                return arena.MakeConcat(Fmt(*n, comments, arena), arena.semi());
              },
              [&](const StructDef* n) { return Fmt(*n, comments, arena); },
              [&](const ConstantDef* n) { return Fmt(*n, comments, arena); },
              [&](const EnumDef* n) { return Fmt(*n, comments, arena); },
              [&](const Import* n) { return Fmt(*n, comments, arena); },
              [&](const ConstAssert* n) {
                return arena.MakeConcat(Fmt(*n, comments, arena), arena.semi());
              }},
      n);
}

DocRef Fmt(const Expr& n, const Comments& comments, DocArena& arena) {
  FmtExprVisitor v(arena, comments);
  XLS_CHECK_OK(n.AcceptExpr(&v));
  DocRef result = v.result();
  if (n.in_parens()) {
    return ConcatNGroup(arena, {arena.oparen(), result, arena.cparen()});
  }
  return result;
}

DocRef Fmt(const Module& n, const Comments& comments, DocArena& arena) {
  std::vector<DocRef> pieces;
  std::optional<Pos> last_member_pos;
  for (size_t i = 0; i < n.top().size(); ++i) {
    const auto& member = n.top()[i];

    const AstNode* node = ToAstNode(member);

    // If this is a desugared proc function, we skip it, and handle formatting
    // it when we get to the proc node.
    if (const Function* f = dynamic_cast<const Function*>(node);
        f != nullptr && f->tag() != Function::Tag::kNormal) {
      continue;
    }

    XLS_VLOG(3) << "Fmt; " << node->GetNodeTypeName()
                << " module member: " << node->ToString();

    // If there are comment blocks between the last member position and the
    // member we're about the process, we need to emit them.
    std::optional<Span> member_span = node->GetSpan();
    XLS_CHECK(member_span.has_value()) << node->GetNodeTypeName();
    Pos member_start = member_span->start();

    // Check the start of this member is >= the last member limit.
    if (last_member_pos.has_value()) {
      XLS_CHECK_GE(member_start, last_member_pos.value()) << node->ToString();
    }

    std::optional<Span> last_comment_span;
    if (std::optional<DocRef> comments_doc =
            EmitCommentsBetween(last_member_pos, member_start, comments, arena,
                                &last_comment_span)) {
      pieces.push_back(comments_doc.value());
      pieces.push_back(arena.hard_line());

      XLS_VLOG(3) << "last_comment_span: " << last_comment_span.value()
                  << " this member start: " << member_start;

      // If the comment abuts the module member we don't put a newline in
      // between, we assume the comment is associated with the member.
      if (last_comment_span->limit().lineno() != member_start.lineno()) {
        pieces.push_back(arena.hard_line());
      }
    }

    // Check the last member position is monotonically increasing.
    if (last_member_pos.has_value()) {
      XLS_CHECK_GT(member_span->limit(), last_member_pos.value());
    }

    last_member_pos = member_span->limit();

    // Here we actually emit the formatted member.
    pieces.push_back(Fmt(member, comments, arena));
    if (i + 1 == n.top().size()) {
      pieces.push_back(arena.hard_line());
    } else {
      pieces.push_back(arena.hard_line());
      pieces.push_back(arena.hard_line());
    }
  }

  if (std::optional<Pos> last_data_limit = comments.last_data_limit();
      last_data_limit.has_value() && last_member_pos < last_data_limit) {
    std::optional<Span> last_comment_span;
    if (std::optional<DocRef> comments_doc =
            EmitCommentsBetween(last_member_pos, last_data_limit.value(),
                                comments, arena, &last_comment_span)) {
      pieces.push_back(comments_doc.value());
      pieces.push_back(arena.hard_line());
    }
  }

  return ConcatN(arena, pieces);
}

std::string AutoFmt(const Module& m, const Comments& comments,
                    int64_t text_width) {
  DocArena arena;
  DocRef ref = Fmt(m, comments, arena);
  return PrettyPrint(arena, ref, text_width);
}

}  // namespace xls::dslx
