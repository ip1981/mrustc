/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/from_ast.cpp
 * - Constructs the HIR module tree from the AST module tree
 */
#include "common.hpp"
#include "hir.hpp"
#include "main_bindings.hpp"
#include <ast/ast.hpp>
#include <ast/expr.hpp> // For shortcut in array size handling
#include <ast/crate.hpp>
#include "from_ast.hpp"
#include "visitor.hpp"
#include <macro_rules/macro_rules.hpp>
#include <hir/item_path.hpp>
#include <limits.h>

::HIR::Module LowerHIR_Module(const ::AST::Module& module, ::HIR::ItemPath path, ::std::vector< ::HIR::SimplePath> traits = {});
::HIR::Function LowerHIR_Function(::HIR::ItemPath path, const ::AST::AttributeList& attrs, const ::AST::Function& f, const ::HIR::TypeRef& self_type);
::HIR::PathParams LowerHIR_PathParams(const Span& sp, const ::AST::PathParams& src_params, bool allow_assoc);
::HIR::TraitPath LowerHIR_TraitPath(const Span& sp, const ::AST::Path& path);

::HIR::SimplePath path_Sized;
RcString    g_core_crate;
RcString    g_crate_name;
::HIR::Crate*   g_crate_ptr = nullptr;
const ::AST::Crate* g_ast_crate_ptr;

// --------------------------------------------------------------------
HIR::LifetimeRef LowerHIR_LifetimeRef(const ::AST::LifetimeRef& r)
{
    return HIR::LifetimeRef(
        // TODO: names?
        r.binding()
        );
}

::HIR::GenericParams LowerHIR_GenericParams(const ::AST::GenericParams& gp, bool* self_is_sized)
{
    ::HIR::GenericParams    rv;

    if( gp.ty_params().size() > 0 )
    {
        for(const auto& tp : gp.ty_params())
        {
            rv.m_types.push_back({ tp.name(), LowerHIR_Type(tp.get_default()), true });
        }
    }
    if( gp.lft_params().size() > 0 )
    {
        for(const auto& lft_def : gp.lft_params())
            rv.m_lifetimes.push_back( HIR::LifetimeDef { lft_def.name().name } );
    }
    if( gp.bounds().size() > 0 )
    {
        for(const auto& bound : gp.bounds())
        {
            TU_MATCH(::AST::GenericBound, (bound), (e),
            (None,
                ),
            (Lifetime,
                rv.m_bounds.push_back(::HIR::GenericBound::make_Lifetime({
                    LowerHIR_LifetimeRef(e.test),
                    LowerHIR_LifetimeRef(e.bound)
                    }));
                ),
            (TypeLifetime,
                rv.m_bounds.push_back(::HIR::GenericBound::make_TypeLifetime({
                    LowerHIR_Type(e.type),
                    LowerHIR_LifetimeRef(e.bound)
                    }));
                ),
            (IsTrait,
                auto type = LowerHIR_Type(e.type);

                // TODO: Check if this trait is `Sized` and ignore if it is

                rv.m_bounds.push_back(::HIR::GenericBound::make_TraitBound({ /*LowerHIR_HigherRankedBounds(e.outer_hrbs),*/ mv$(type), LowerHIR_TraitPath(bound.span, e.trait) }));
                //rv.m_bounds.back().as_TraitBound().trait.m_hrls = LowerHIR_HigherRankedBounds(e.inner_hrbs);
                ),
            (MaybeTrait,
                auto type = LowerHIR_Type(e.type);
                if( ! type.m_data.is_Generic() )
                    BUG(bound.span, "MaybeTrait on non-param - " << type);
                const auto& ge = type.m_data.as_Generic();
                const auto& param_name = ge.name;
                unsigned param_idx;
                if( ge.binding == 0xFFFF ) {
                    if( !self_is_sized ) {
                        BUG(bound.span, "MaybeTrait on parameter on Self when not allowed");
                    }
                    param_idx = 0xFFFF;
                }
                else {
                    param_idx = ::std::find_if( rv.m_types.begin(), rv.m_types.end(), [&](const auto& x) { return x.m_name == param_name; } ) - rv.m_types.begin();
                    if( param_idx >= rv.m_types.size() ) {
                        BUG(bound.span, "MaybeTrait on parameter not in parameter list (#" << ge.binding << " " << param_name << ")");
                    }
                }

                // Compare with list of known default traits (just Sized atm) and set a marker
                auto trait = LowerHIR_GenericPath(bound.span, e.trait);
                if( trait.m_path == path_Sized ) {
                    if( param_idx == 0xFFFF ) {
                        assert( self_is_sized );
                        *self_is_sized = false;
                    }
                    else {
                        assert( param_idx < rv.m_types.size() );
                        rv.m_types[param_idx].m_is_sized = false;
                    }
                }
                else {
                    ERROR(bound.span, E0000, "MaybeTrait on unknown trait " << trait.m_path);
                }
                ),
            (NotTrait,
                TODO(bound.span, "Negative trait bounds");
                ),

            (Equality,
                rv.m_bounds.push_back(::HIR::GenericBound::make_TypeEquality({
                    LowerHIR_Type(e.type),
                    LowerHIR_Type(e.replacement)
                    }));
                )
            )
        }
    }

    return rv;
}

::HIR::Pattern LowerHIR_Pattern(const ::AST::Pattern& pat)
{
    TRACE_FUNCTION_F("@" << pat.span().filename << ":" << pat.span().start_line << " pat = " << pat);

    ::HIR::PatternBinding   binding;
    if( pat.binding().is_valid() )
    {
        ::HIR::PatternBinding::Type bt = ::HIR::PatternBinding::Type::Move;
        switch(pat.binding().m_type)
        {
        case ::AST::PatternBinding::Type::MOVE: bt = ::HIR::PatternBinding::Type::Move; break;
        case ::AST::PatternBinding::Type::REF:  bt = ::HIR::PatternBinding::Type::Ref;  break;
        case ::AST::PatternBinding::Type::MUTREF: bt = ::HIR::PatternBinding::Type::MutRef; break;
        }
        binding = ::HIR::PatternBinding(pat.binding().m_mutable, bt, pat.binding().m_name.name, pat.binding().m_slot);
    }

    struct H {
        static ::std::vector< ::HIR::Pattern>   lowerhir_patternvec(const ::std::vector< ::AST::Pattern>& sub_patterns) {
            ::std::vector< ::HIR::Pattern>  rv;
            for(const auto& sp : sub_patterns)
                rv.push_back( LowerHIR_Pattern(sp) );
            return rv;
        }
    };

    TU_MATCH_HDRA( (pat.data()), {)
    TU_ARMA(MaybeBind, e) {
        BUG(pat.span(), "Encountered MaybeBind pattern");
        }
    TU_ARMA(Macro, e) {
        BUG(pat.span(), "Encountered Macro pattern");
        }
    TU_ARMA(Any, e)
        return ::HIR::Pattern {
            mv$(binding),
            ::HIR::Pattern::Data::make_Any({})
            };
    TU_ARMA(Box, e)
        return ::HIR::Pattern {
            mv$(binding),
            ::HIR::Pattern::Data::make_Box({
                box$(LowerHIR_Pattern( *e.sub ))
                })
            };
    TU_ARMA(Ref, e)
        return ::HIR::Pattern {
            mv$(binding),
            ::HIR::Pattern::Data::make_Ref({
                (e.mut ? ::HIR::BorrowType::Unique : ::HIR::BorrowType::Shared),
                box$(LowerHIR_Pattern( *e.sub ))
                })
            };
    TU_ARMA(Tuple, e) {
        auto leading  = H::lowerhir_patternvec( e.start );
        auto trailing = H::lowerhir_patternvec( e.end   );

        if( e.has_wildcard )
        {
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_SplitTuple({
                    mv$(leading), mv$(trailing)
                    })
                };
        }
        else
        {
            assert( trailing.size() == 0 );
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_Tuple({
                    mv$(leading)
                    })
                };
        }
        }
    TU_ARMA(StructTuple, e) {
        unsigned int leading_count  = e.tup_pat.start.size();
        unsigned int trailing_count = e.tup_pat.end  .size();
        TU_MATCH_HDRA( (e.path.m_bindings.value), {)
        default:
            BUG(pat.span(), "Encountered StructTuple pattern not pointing to a enum variant or a struct - " << e.path);
        TU_ARMA(EnumVar, pb) {
            assert( pb.enum_ || pb.hir );
            unsigned int field_count;
            if( pb.enum_ ) {
                const auto& var = pb.enum_->variants()[pb.idx].m_data;
                field_count = var.as_Tuple().m_sub_types.size();
            }
            else {
                const auto& var = pb.hir->m_data.as_Data().at(pb.idx);
                // Need to be able to look up the type's actual definition
                // - Either a name lookup, or have binding be done before this pass.
                const auto& str = g_crate_ptr->get_struct_by_path(pat.span(), var.type.m_data.as_Path().path.m_data.as_Generic().m_path);
                field_count = str.m_data.as_Tuple().size();
                //field_count = var.type.m_data.as_Path().binding.as_Struct()->m_data.as_Tuple().size();
            }
            ::std::vector<HIR::Pattern> sub_patterns;

            if( e.tup_pat.has_wildcard ) {
                sub_patterns.reserve( field_count );
                if( leading_count + trailing_count > field_count ) {
                    ERROR(pat.span(), E0000, "Enum variant pattern has too many fields - " << field_count << " max, got " << leading_count + trailing_count);
                }
                unsigned int padding_count = field_count - leading_count - trailing_count;
                for(const auto& subpat : e.tup_pat.start) {
                    sub_patterns.push_back( LowerHIR_Pattern(subpat) );
                }
                for(unsigned int i = 0; i < padding_count; i ++) {
                    sub_patterns.push_back( ::HIR::Pattern() );
                }
                for(const auto& subpat : e.tup_pat.end) {
                    sub_patterns.push_back( LowerHIR_Pattern(subpat) );
                }
            }
            else {
                assert( trailing_count == 0 );

                if( leading_count != field_count ) {
                    ERROR(pat.span(), E0000, "Enum variant pattern has a mismatched field count - " << field_count << " exp, got " << leading_count);
                }
                sub_patterns = H::lowerhir_patternvec( e.tup_pat.start );
            }

            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_EnumTuple({
                    LowerHIR_GenericPath(pat.span(), e.path),
                    nullptr, 0,
                    mv$(sub_patterns)
                    })
                };
            }
        TU_ARMA(Struct, pb) {
            assert( pb.struct_ || pb.hir );
            unsigned int field_count;
            if( pb.struct_ ) {
                if( !pb.struct_->m_data.is_Tuple() )
                    ERROR(pat.span(), E0000, "Tuple struct pattern on non-tuple struct - " << e.path);
                field_count = pb.struct_->m_data.as_Tuple().ents.size();
            }
            else {
                if( !pb.hir->m_data.is_Tuple() )
                    ERROR(pat.span(), E0000, "Tuple struct pattern on non-tuple struct - " << e.path);
                field_count = pb.hir->m_data.as_Tuple().size();
            }
            ::std::vector<HIR::Pattern> sub_patterns;

            if( e.tup_pat.has_wildcard ) {
                sub_patterns.reserve( field_count );
                if( leading_count + trailing_count > field_count ) {
                    ERROR(pat.span(), E0000, "Struct pattern has too many fields - " << field_count << " max, got " << leading_count + trailing_count);
                }
                unsigned int padding_count = field_count - leading_count - trailing_count;
                for(const auto& subpat : e.tup_pat.start) {
                    sub_patterns.push_back( LowerHIR_Pattern(subpat) );
                }
                for(unsigned int i = 0; i < padding_count; i ++) {
                    sub_patterns.push_back( ::HIR::Pattern() );
                }
                for(const auto& subpat : e.tup_pat.end) {
                    sub_patterns.push_back( LowerHIR_Pattern(subpat) );
                }
            }
            else {
                assert( trailing_count == 0 );

                if( leading_count != field_count ) {
                    ERROR(pat.span(), E0000, "Struct pattern has a mismatched field count - " << field_count << " exp, got " << leading_count);
                }
                sub_patterns = H::lowerhir_patternvec( e.tup_pat.start );
            }

            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_StructTuple({
                    LowerHIR_GenericPath(pat.span(), e.path),
                    nullptr,
                    mv$(sub_patterns)
                    })
                };
            }
        }
        }
    TU_ARMA(Struct, e) {
        ::std::vector< ::std::pair< RcString, ::HIR::Pattern> > sub_patterns;
        for(const auto& sp : e.sub_patterns)
            sub_patterns.push_back( ::std::make_pair(sp.first, LowerHIR_Pattern(sp.second)) );

        if( e.sub_patterns.empty() && !e.is_exhaustive ) {
            if( e.path.m_bindings.value.is_EnumVar() ) {
                return ::HIR::Pattern {
                    mv$(binding),
                    ::HIR::Pattern::Data::make_EnumStruct({
                        LowerHIR_GenericPath(pat.span(), e.path),
                        nullptr, 0,
                        mv$(sub_patterns),
                        e.is_exhaustive
                        })
                    };
            }
        }

        TU_MATCH_HDRA( (e.path.m_bindings.type), {)
        default:
            BUG(pat.span(), "Encountered Struct pattern not pointing to a enum variant or a struct - " << e.path);
        TU_ARMA(EnumVar, pb) {
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_EnumStruct({
                    LowerHIR_GenericPath(pat.span(), e.path),
                    nullptr, 0,
                    mv$(sub_patterns),
                    e.is_exhaustive
                    })
                };
            }
        TU_ARMA(TypeAlias, pb) {
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_Struct({
                    LowerHIR_GenericPath(pat.span(), e.path),
                    nullptr,
                    mv$(sub_patterns),
                    e.is_exhaustive
                    })
                };
            }
        TU_ARMA(Struct, pb) {
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_Struct({
                    LowerHIR_GenericPath(pat.span(), e.path),
                    nullptr,
                    mv$(sub_patterns),
                    e.is_exhaustive
                    })
                };
            }
        }
        }

    TU_ARMA(Value, e) {
        struct H {
            static ::HIR::CoreType get_int_type(const Span& sp, const ::eCoreType ct) {
                switch(ct)
                {
                case CORETYPE_ANY:  return ::HIR::CoreType::Str;

                case CORETYPE_I8 :  return ::HIR::CoreType::I8;
                case CORETYPE_U8 :  return ::HIR::CoreType::U8;
                case CORETYPE_I16:  return ::HIR::CoreType::I16;
                case CORETYPE_U16:  return ::HIR::CoreType::U16;
                case CORETYPE_I32:  return ::HIR::CoreType::I32;
                case CORETYPE_U32:  return ::HIR::CoreType::U32;
                case CORETYPE_I64:  return ::HIR::CoreType::I64;
                case CORETYPE_U64:  return ::HIR::CoreType::U64;

                case CORETYPE_INT:  return ::HIR::CoreType::Isize;
                case CORETYPE_UINT: return ::HIR::CoreType::Usize;

                case CORETYPE_CHAR: return ::HIR::CoreType::Char;

                case CORETYPE_BOOL: return ::HIR::CoreType::Bool;

                default:
                    BUG(sp, "Unknown type for integer literal in pattern - " << ct );
                }
            }
            static ::HIR::CoreType get_float_type(const Span& sp, const ::eCoreType ct) {
                switch(ct)
                {
                case CORETYPE_ANY:  return ::HIR::CoreType::Str;
                case CORETYPE_F32:  return ::HIR::CoreType::F32;
                case CORETYPE_F64:  return ::HIR::CoreType::F64;
                default:
                    BUG(sp, "Unknown type for float literal in pattern - " << ct );
                }
            }
            static ::HIR::Pattern::Value lowerhir_pattern_value(const Span& sp, const ::AST::Pattern::Value& v) {
                TU_MATCH(::AST::Pattern::Value, (v), (e),
                (Invalid,
                    BUG(sp, "Encountered Invalid value in Pattern");
                    ),
                (Integer,
                    return ::HIR::Pattern::Value::make_Integer({
                        H::get_int_type(sp, e.type),
                        e.value
                        });
                    ),
                (Float,
                    return ::HIR::Pattern::Value::make_Float({
                        H::get_float_type(sp, e.type),
                        e.value
                        });
                    ),
                (String,
                    return ::HIR::Pattern::Value::make_String(e);
                    ),
                (ByteString,
                    return ::HIR::Pattern::Value::make_ByteString({e.v});
                    ),
                (Named,
                    return ::HIR::Pattern::Value::make_Named( {LowerHIR_Path(sp, e), nullptr} );
                    )
                )
                throw "BUGCHECK: Reached end of LowerHIR_Pattern::H::lowerhir_pattern_value";
            }
        };
        if( e.end.is_Invalid() ) {
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_Value({
                    H::lowerhir_pattern_value(pat.span(), e.start)
                    })
                };
        }
        else {
            return ::HIR::Pattern {
                mv$(binding),
                ::HIR::Pattern::Data::make_Range({
                    H::lowerhir_pattern_value(pat.span(), e.start),
                    H::lowerhir_pattern_value(pat.span(), e.end)
                    })
                };
        }
        }
    TU_ARMA(Slice, e) {
        ::std::vector< ::HIR::Pattern>  leading;
        for(const auto& sp : e.sub_pats)
            leading.push_back( LowerHIR_Pattern(sp) );
        return ::HIR::Pattern {
            mv$(binding),
            ::HIR::Pattern::Data::make_Slice({
                mv$(leading)
                })
            };
        }
    TU_ARMA(SplitSlice, e) {
        ::std::vector< ::HIR::Pattern>  leading;
        for(const auto& sp : e.leading)
            leading.push_back( LowerHIR_Pattern(sp) );

        ::std::vector< ::HIR::Pattern>  trailing;
        for(const auto& sp : e.trailing)
            trailing.push_back( LowerHIR_Pattern(sp) );

        auto extra_bind = e.extra_bind.is_valid()
            // TODO: Share code with the outer binding code
            ? ::HIR::PatternBinding(false, ::HIR::PatternBinding::Type::Ref, e.extra_bind.m_name.name, e.extra_bind.m_slot)
            : ::HIR::PatternBinding()
            ;

        return ::HIR::Pattern {
            mv$(binding),
            ::HIR::Pattern::Data::make_SplitSlice({
                mv$(leading),
                mv$(extra_bind),
                mv$(trailing)
                })
            };
        }
    }
    throw "unreachable";
}

::HIR::ExprPtr LowerHIR_Expr(const ::std::shared_ptr< ::AST::ExprNode>& e)
{
    if( e.get() ) {
        return LowerHIR_ExprNode(*e);
    }
    else {
        return ::HIR::ExprPtr();
    }
}
::HIR::ExprPtr LowerHIR_Expr(const ::AST::Expr& e)
{
    if( e.is_valid() ) {
        return LowerHIR_ExprNode(e.node());
    }
    else {
        return ::HIR::ExprPtr();
    }
}

::HIR::SimplePath LowerHIR_SimplePath(const Span& sp, const ::AST::Path& path, bool allow_final_generic)
{
    TU_IFLET(::AST::Path::Class, path.m_class, Absolute, e,
        ::HIR::SimplePath   rv( e.crate );
        if( rv.m_crate_name == "" )
            rv.m_crate_name = g_crate_name;
        for( const auto& node : e.nodes )
        {
            if( ! node.args().is_empty() )
            {
                if( allow_final_generic && &node == &e.nodes.back() ) {
                    // Let it pass
                }
                else {
                    throw "BUG: Encountered path with parameters when creating ::HIR::GenericPath";
                }
            }

            rv.m_components.push_back( node.name() );
        }
        return rv;
    )
    else {
        throw "BUG: Encountered non-Absolute path when creating ::HIR::GenericPath";
    }
}
::HIR::PathParams LowerHIR_PathParams(const Span& sp, const ::AST::PathParams& src_params, bool allow_assoc)
{
    ::HIR::PathParams   params;

    // TODO: Lifetime params (not encoded in ::HIR::PathNode as yet)
    //for(const auto& param : src_params.m_lifetimes) {
    //}

    for(const auto& param : src_params.m_types) {
        params.m_types.push_back( LowerHIR_Type(param) );
    }

    // Leave 'm_assoc' alone?
    if( !allow_assoc && src_params.m_assoc.size() > 0 )
    {
        BUG(sp, "Encountered path parameters with associated type bounds where they are not allowed");
    }

    return params;
}
::HIR::GenericPath LowerHIR_GenericPath(const Span& sp, const ::AST::Path& path, bool allow_assoc)
{
    TU_IFLET(::AST::Path::Class, path.m_class, Absolute, e,
        auto simpepath = LowerHIR_SimplePath(sp, path, true);
        ::HIR::PathParams   params = LowerHIR_PathParams(sp, e.nodes.back().args(), allow_assoc);
        auto rv = ::HIR::GenericPath(mv$(simpepath), mv$(params));
        DEBUG(path << " => " << rv);
        return rv;
    )
    else {
        if(const auto* e = path.m_class.opt_UFCS()) {
            DEBUG(path);
            if( !e->type ) {
            }
            //else if( e->trait ) {
            //}
            else if( ! e->nodes.empty() ) {
            }
            else if( !e->type->m_data.is_Path() ) {
            }
            else {
                // HACK: `Self` replacement
                return LowerHIR_GenericPath(sp, e->type->m_data.as_Path().path, false);
            }
        }
        BUG(sp, "Encountered non-Absolute path when creating ::HIR::GenericPath - " << path);
    }
}
::HIR::TraitPath LowerHIR_TraitPath(const Span& sp, const ::AST::Path& path)
{
    ::HIR::TraitPath    rv {
        LowerHIR_GenericPath(sp, path, true),
        {},
        {},
        nullptr
        };

    for(const auto& assoc : path.nodes().back().args().m_assoc)
    {
        rv.m_type_bounds.insert(::std::make_pair( assoc.first, LowerHIR_Type(assoc.second) ));
    }

    return rv;
}
::HIR::Path LowerHIR_Path(const Span& sp, const ::AST::Path& path)
{
    TU_MATCH(::AST::Path::Class, (path.m_class), (e),
    (Invalid,
        BUG(sp, "BUG: Encountered Invalid path in LowerHIR_Path");
        ),
    (Local,
        TODO(sp, "What to do wth Path::Class::Local in LowerHIR_Path - " << path);
        ),
    (Relative,
        BUG(sp, "Encountered `Relative` path in LowerHIR_Path - " << path);
        ),
    (Self,
        BUG(sp, "Encountered `Self` path in LowerHIR_Path - " << path);
        ),
    (Super,
        BUG(sp, "Encountered `Super` path in LowerHIR_Path - " << path);
        ),
    (Absolute,
        return ::HIR::Path( LowerHIR_GenericPath(sp, path) );
        ),
    (UFCS,
        if( e.nodes.size() == 0 )
        {
            if( !e.trait )
                TODO(sp, "Handle UFCS inherent and no nodes - " << path);
            if( e.trait->is_valid() )
                TODO(sp, "Handle UFCS w/ trait and no nodes - " << path);
            auto type = LowerHIR_Type(*e.type);
            ASSERT_BUG(sp, type.m_data.is_Path(), "No nodes and non-Path type - " << path);
            return mv$(type.m_data.as_Path().path);
        }
        if( e.nodes.size() > 1 )
            TODO(sp, "Handle UFCS with multiple nodes - " << path);
        // - No associated type bounds allowed in UFCS paths
        auto params = LowerHIR_PathParams(sp, e.nodes.front().args(), false);
        if( ! e.trait )
        {
            auto type = box$( LowerHIR_Type(*e.type) );
            if( type->m_data.is_Generic() ) {
                BUG(sp, "Generics can't be used with UfcsInherent - " << path);
            }
            return ::HIR::Path(::HIR::Path::Data::make_UfcsInherent({
                mv$(type),
                e.nodes[0].name(),
                mv$(params)
                }));
        }
        else if( ! e.trait->is_valid() )
        {
            return ::HIR::Path(::HIR::Path::Data::make_UfcsUnknown({
                box$( LowerHIR_Type(*e.type) ),
                e.nodes[0].name(),
                mv$(params)
                }));
        }
        else
        {
            return ::HIR::Path(::HIR::Path::Data::make_UfcsKnown({
                box$(LowerHIR_Type(*e.type)),
                LowerHIR_GenericPath(sp, *e.trait),
                e.nodes[0].name(),
                mv$(params)
                }));
        }
        )
    )
    throw "BUGCHECK: Reached end of LowerHIR_Path";
}

::HIR::TypeRef LowerHIR_Type(const ::TypeRef& ty)
{
    TU_MATCH(::TypeData, (ty.m_data), (e),
    (None,
        BUG(ty.span(), "TypeData::None");
        ),
    (Bang,
        // Aka diverging
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Diverge({}) );
        ),
    (Any,
        return ::HIR::TypeRef();
        ),
    (Unit,
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Tuple({}) );
        ),
    (Macro,
        BUG(ty.span(), "TypeData::Macro");
        ),
    (Primitive,
        switch(e.core_type)
        {
        case CORETYPE_BOOL: return ::HIR::TypeRef( ::HIR::CoreType::Bool );
        case CORETYPE_CHAR: return ::HIR::TypeRef( ::HIR::CoreType::Char );
        case CORETYPE_STR : return ::HIR::TypeRef( ::HIR::CoreType::Str );
        case CORETYPE_F32:  return ::HIR::TypeRef( ::HIR::CoreType::F32 );
        case CORETYPE_F64:  return ::HIR::TypeRef( ::HIR::CoreType::F64 );

        case CORETYPE_I8 :  return ::HIR::TypeRef( ::HIR::CoreType::I8 );
        case CORETYPE_U8 :  return ::HIR::TypeRef( ::HIR::CoreType::U8 );
        case CORETYPE_I16:  return ::HIR::TypeRef( ::HIR::CoreType::I16 );
        case CORETYPE_U16:  return ::HIR::TypeRef( ::HIR::CoreType::U16 );
        case CORETYPE_I32:  return ::HIR::TypeRef( ::HIR::CoreType::I32 );
        case CORETYPE_U32:  return ::HIR::TypeRef( ::HIR::CoreType::U32 );
        case CORETYPE_I64:  return ::HIR::TypeRef( ::HIR::CoreType::I64 );
        case CORETYPE_U64:  return ::HIR::TypeRef( ::HIR::CoreType::U64 );

        case CORETYPE_I128: return ::HIR::TypeRef( ::HIR::CoreType::I128 );
        case CORETYPE_U128: return ::HIR::TypeRef( ::HIR::CoreType::U128 );

        case CORETYPE_INT:  return ::HIR::TypeRef( ::HIR::CoreType::Isize );
        case CORETYPE_UINT: return ::HIR::TypeRef( ::HIR::CoreType::Usize );
        case CORETYPE_ANY:
            TODO(ty.span(), "TypeData::Primitive - CORETYPE_ANY");
        case CORETYPE_INVAL:
            BUG(ty.span(), "TypeData::Primitive - CORETYPE_INVAL");
        }
        ),
    (Tuple,
        ::HIR::TypeRef::Data::Data_Tuple v;
        for( const auto& st : e.inner_types )
        {
            v.push_back( LowerHIR_Type(st) );
        }
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Tuple(mv$(v)) );
        ),
    (Borrow,
        auto cl = (e.is_mut ? ::HIR::BorrowType::Unique : ::HIR::BorrowType::Shared);
        return ::HIR::TypeRef::new_borrow( cl, LowerHIR_Type(*e.inner) );
        ),
    (Pointer,
        auto cl = (e.is_mut ? ::HIR::BorrowType::Unique : ::HIR::BorrowType::Shared);
        return ::HIR::TypeRef::new_pointer( cl, LowerHIR_Type(*e.inner) );
        ),
    (Array,
        auto inner = LowerHIR_Type(*e.inner);
        if( e.size ) {
            // If the size expression is an unannotated or usize integer literal, don't bother converting the expression
            if( const auto* ptr = dynamic_cast<const ::AST::ExprNode_Integer*>(&*e.size) )
            {
                if( ptr->m_datatype == CORETYPE_UINT || ptr->m_datatype == CORETYPE_ANY )
                {
                    // TODO: Chage the HIR format to support very large arrays
                    if( ptr->m_value >= UINT64_MAX ) {
                        ERROR(ty.span(), E0000, "Array size out of bounds - 0x" << ::std::hex << ptr->m_value << " > 0x" << UINT64_MAX << " in " << ::std::dec << ty);
                    }
                    auto size_val = static_cast<unsigned int>( ptr->m_value );
                    return ::HIR::TypeRef::new_array( mv$(inner), size_val );
                }
            }

            return ::HIR::TypeRef::new_array( mv$(inner), LowerHIR_Expr(e.size) );
        }
        else {
            return ::HIR::TypeRef::new_slice( mv$(inner) );
        }
        ),

    (Path,
        TU_IFLET(::AST::Path::Class, e.path.m_class, Local, l,
            unsigned int slot;
            // NOTE: TypeParameter is unused
            if( const auto* p = e.path.m_bindings.value.opt_Variable() ) {
                slot = p->slot;
            }
            else {
                BUG(ty.span(), "Unbound local encountered in " << e.path);
            }
            return ::HIR::TypeRef( l.name, slot );
        )
        else {
            return ::HIR::TypeRef( LowerHIR_Path(ty.span(), e.path) );
        }
        ),
    (TraitObject,
        ::HIR::TypeRef::Data::Data_TraitObject  v;
        // TODO: Lifetime
        for(const auto& t : e.traits)
        {
            DEBUG("t = " << t.path);
            const auto& tb = t.path.m_bindings.type.as_Trait();
            assert( tb.trait_ || tb.hir );
            if( (tb.trait_ ? tb.trait_->is_marker() : tb.hir->m_is_marker) )
            {
                if( tb.hir ) {
                    DEBUG(tb.hir->m_values.size());
                }
                // TODO: If this has HRBs, what?
                v.m_markers.push_back( LowerHIR_GenericPath(ty.span(), t.path) );
            }
            else {
                // TraitPath -> GenericPath -> SimplePath
                if( v.m_trait.m_path.m_path.m_components.size() > 0 ) {
                    ERROR(ty.span(), E0000, "Multiple data traits in trait object - " << ty);
                }
                // TODO: Handle HRBs
                v.m_trait = LowerHIR_TraitPath(ty.span(), t.path);
            }
        }
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_TraitObject( mv$(v) ) );
        ),
    (ErasedType,
        ASSERT_BUG(ty.span(), e.traits.size() > 0, "ErasedType with no traits");

        ::std::vector< ::HIR::TraitPath>    traits;
        for(const auto& t : e.traits)
        {
            DEBUG("t = " << t.path);
            // TODO: Pass the HRBs down
            traits.push_back( LowerHIR_TraitPath(ty.span(), t.path) );
        }
        ::HIR::LifetimeRef  lft;
        if( e.lifetimes.size() == 0 )
        {
        }
        else if( e.lifetimes.size() == 1 )
        {
            // TODO: Convert the lifetime reference
        }
        else
        {
            TODO(ty.span(), "Handle multiple lifetime parameters - " << ty);
        }
        // Leave `m_origin` until the bind pass
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_ErasedType(::HIR::TypeRef::Data::Data_ErasedType {
            ::HIR::Path(::HIR::SimplePath()), 0,
            mv$(traits),
            lft
            } ) );
        ),
    (Function,
        ::std::vector< ::HIR::TypeRef>  args;
        for(const auto& arg : e.info.m_arg_types)
            args.push_back( LowerHIR_Type(arg) );
        ::HIR::FunctionType f {
            e.info.is_unsafe,
            e.info.m_abi,
            box$( LowerHIR_Type(*e.info.m_rettype) ),
            mv$(args)   // TODO: e.info.is_variadic
            };
        if( f.m_abi == "" )
            f.m_abi = ABI_RUST;
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Function( mv$(f) ) );
        ),
    (Generic,
        assert(e.index < 0x10000);
        return ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Generic({ e.name, e.index }) );
        )
    )
    throw "BUGCHECK: Reached end of LowerHIR_Type";
}

::HIR::TypeAlias LowerHIR_TypeAlias(const ::AST::TypeAlias& ta)
{
    return ::HIR::TypeAlias {
        LowerHIR_GenericParams(ta.params(), nullptr),
        LowerHIR_Type(ta.type())
        };
}


namespace {
    template<typename T>
    ::HIR::VisEnt<T> new_visent(HIR::Publicity pub, T v) {
        return ::HIR::VisEnt<T> { pub, mv$(v) };
    }

    ::HIR::SimplePath get_parent_module(const ::HIR::ItemPath& p) {
        const ::HIR::ItemPath*  parent_ip = p.parent;
        assert(parent_ip);
        while(parent_ip->name && parent_ip->name[0] == '#')
        {
            parent_ip = parent_ip->parent;
            assert(parent_ip);
        }
        return parent_ip->get_simple_path();
    }
}

::HIR::Struct LowerHIR_Struct(::HIR::ItemPath path, const ::AST::Struct& ent, const ::AST::AttributeList& attrs)
{
    TRACE_FUNCTION_F(path);
    ::HIR::Struct::Data data;

    auto priv_path = ::HIR::Publicity::new_priv( get_parent_module(path) );
    auto get_pub = [&](bool is_pub){ return is_pub ? ::HIR::Publicity::new_global() : priv_path; };

    TU_MATCH(::AST::StructData, (ent.m_data), (e),
    (Unit,
        data = ::HIR::Struct::Data::make_Unit({});
        ),
    (Tuple,
        ::HIR::Struct::Data::Data_Tuple fields;

        for(const auto& field : e.ents)
            fields.push_back( { get_pub(field.m_is_public), LowerHIR_Type(field.m_type) } );

        data = ::HIR::Struct::Data::make_Tuple( mv$(fields) );
        ),
    (Struct,
        ::HIR::Struct::Data::Data_Named fields;
        for(const auto& field : e.ents)
            fields.push_back( ::std::make_pair( field.m_name, new_visent( get_pub(field.m_is_public), LowerHIR_Type(field.m_type)) ) );
        data = ::HIR::Struct::Data::make_Named( mv$(fields) );
        )
    )

    auto rv = ::HIR::Struct {
        LowerHIR_GenericParams(ent.params(), nullptr),
        ::HIR::Struct::Repr::Rust,
        mv$(data)
        };

    if( const auto* attr_repr = attrs.get("repr") )
    {
        ASSERT_BUG(attr_repr->span(), attr_repr->has_sub_items(), "#[repr] attribute malformed, " << *attr_repr);
        ASSERT_BUG(attr_repr->span(), attr_repr->items().size() > 0, "#[repr] attribute malformed, " << *attr_repr);
        // TODO: Change reprs to be a flag set (instead of an enum)?
        // (Or at least make C be a flag)
        for( const auto& a : attr_repr->items() )
        {
            const auto& repr_str = a.name();
            if( repr_str == "C" ) {
                ASSERT_BUG(a.span(), a.has_noarg(), "#[repr] attribute malformed, " << *attr_repr);
                if( rv.m_repr == ::HIR::Struct::Repr::Aligned )
                {
                }
                else if( rv.m_repr != ::HIR::Struct::Repr::Packed )
                {
                    ASSERT_BUG(a.span(), rv.m_repr == ::HIR::Struct::Repr::Rust, "Conflicting #[repr] attributes - " << rv.m_repr << ", " << repr_str);
                    rv.m_repr = ::HIR::Struct::Repr::C;
                }
            }
            else if( repr_str == "packed" ) {
                ASSERT_BUG(a.span(), a.has_noarg(), "#[repr] attribute malformed, " << *attr_repr);
                ASSERT_BUG(a.span(), rv.m_repr == ::HIR::Struct::Repr::Rust || rv.m_repr == ::HIR::Struct::Repr::C, "Conflicting #[repr] attributes - " << rv.m_repr << ", " << repr_str);
                rv.m_repr = ::HIR::Struct::Repr::Packed;
            }
            else if( repr_str == "simd" ) {
                ASSERT_BUG(a.span(), a.has_noarg(), "#[repr] attribute malformed, " << *attr_repr);
                ASSERT_BUG(a.span(), rv.m_repr == ::HIR::Struct::Repr::Rust, "Conflicting #[repr] attributes - " << rv.m_repr << ", " << repr_str);
                rv.m_repr = ::HIR::Struct::Repr::Simd;
            }
            else if( repr_str == "transparent" ) {
                ASSERT_BUG(a.span(), a.has_noarg(), "#[repr] attribute malformed, " << *attr_repr);
                ASSERT_BUG(a.span(), rv.m_repr == ::HIR::Struct::Repr::Rust, "Conflicting #[repr] attributes - " << rv.m_repr << ", " << repr_str);
                rv.m_repr = ::HIR::Struct::Repr::Transparent;
            }
            else if( repr_str == "align" ) {
                //ASSERT_BUG(a.span(), a.has_string(), "#[repr(aligned)] attribute malformed, " << *attr_repr);
                ASSERT_BUG(a.span(), rv.m_repr != ::HIR::Struct::Repr::Packed, "Conflicting #[repr] attributes - " << rv.m_repr << ", " << repr_str);
                //rv.m_repr = ::HIR::Struct::Repr::Aligned;
                //rv.m_forced_alignment = ::std::stol(a.string());
            }
            else {
                TODO(a.span(), "Handle struct repr '" << repr_str << "'");
            }
        }
    }

    return rv;
}

::HIR::Enum LowerHIR_Enum(::HIR::ItemPath path, const ::AST::Enum& ent, const ::AST::AttributeList& attrs, ::std::function<void(RcString, ::HIR::Struct)> push_struct)
{
    // 1. Figure out what sort of enum this is (value or data)
    bool has_value = false;
    bool has_data = false;
    for(const auto& var : ent.variants())
    {
        if( TU_TEST1(var.m_data, Value, .m_value.is_valid()) )
        {
            has_value = true;
        }
        else if( var.m_data.is_Tuple() || var.m_data.is_Struct() )
        {
            has_data = true;
        }
        else
        {
            // Unit-like
            assert(var.m_data.is_Value());
        }
    }

    if( has_value && has_data )
    {
        ERROR(Span(), E0000, "Enum " << path << " has both value and data variants");
    }

    ::HIR::Enum::Class  data;
    if( ent.variants().size() > 0 && !has_data )
    {
        ::std::vector<::HIR::Enum::ValueVariant>    variants;
        for(const auto& var : ent.variants())
        {
            const auto& ve = var.m_data.as_Value();
            // TODO: Quick consteval on the expression?
            variants.push_back({
                var.m_name, LowerHIR_Expr(ve.m_value), 0
                });
        }

        auto repr = ::HIR::Enum::Repr::Rust;
        if( const auto* attr_repr = attrs.get("repr") )
        {
            ASSERT_BUG(Span(), attr_repr->has_sub_items(), "#[repr] attribute malformed, " << *attr_repr);
            ASSERT_BUG(Span(), attr_repr->items().size() == 1, "#[repr] attribute malformed, " << *attr_repr);
            ASSERT_BUG(Span(), attr_repr->items()[0].has_noarg(), "#[repr] attribute malformed, " << *attr_repr);
            const auto& repr_str = attr_repr->items()[0].name();
            if( repr_str == "C" ) {
                repr = ::HIR::Enum::Repr::C;
            }
            else if( repr_str == "u8") {
                repr = ::HIR::Enum::Repr::U8;
            }
            else if( repr_str == "u16") {
                repr = ::HIR::Enum::Repr::U16;
            }
            else if( repr_str == "u32") {
                repr = ::HIR::Enum::Repr::U32;
            }
            else if( repr_str == "u64") {
                repr = ::HIR::Enum::Repr::U64;
            }
            else if( repr_str == "usize") {
                repr = ::HIR::Enum::Repr::Usize;
            }
            else {
                ERROR(Span(), E0000, "Unknown enum repr '" << repr_str << "'");
            }
        }
        data = ::HIR::Enum::Class::make_Value({ repr, mv$(variants) });
    }
    // NOTE: empty enums are encoded as empty Data enums
    else
    {
        ::std::vector<::HIR::Enum::DataVariant>    variants;
        for(const auto& var : ent.variants())
        {
            if( var.m_data.is_Value() )
            {
                // TODO: Should this make its own unit-like struct?
                variants.push_back({ var.m_name, false, ::HIR::TypeRef::new_unit() });
            }
            //else if( TU_TEST1(var.m_data, Tuple, m_sub_types.size() == 0) )
            //{
            //    variants.push_back({ var.m_name, false, ::HIR::TypeRef::new_unit() });
            //}
            //else if( TU_TEST1(var.m_data, Tuple, m_sub_types.size() == 1) )
            //{
            //    const auto& ty = var.m_data.as_Tuple().m_sub_types[0];
            //    variants.push_back({ var.m_name, false, LowerHIR_Type(ty) });
            //}
            else
            {
                ::HIR::Struct::Data data;
                if( const auto* ve = var.m_data.opt_Tuple() )
                {
                    ::HIR::Struct::Data::Data_Tuple fields;
                    for(const auto& field : ve->m_sub_types)
                        fields.push_back( new_visent(::HIR::Publicity::new_global(), LowerHIR_Type(field)) );
                    data = ::HIR::Struct::Data::make_Tuple( mv$(fields) );
                }
                else if( const auto* ve = var.m_data.opt_Struct() )
                {
                    ::HIR::Struct::Data::Data_Named fields;
                    for(const auto& field : ve->m_fields)
                        fields.push_back( ::std::make_pair( field.m_name, new_visent(::HIR::Publicity::new_global(), LowerHIR_Type(field.m_type)) ) );
                    data = ::HIR::Struct::Data::make_Named( mv$(fields) );
                }
                else
                {
                    throw "";
                }

                auto ty_name = RcString::new_interned(FMT(path.name << "#" << var.m_name));
                push_struct(
                    ty_name,
                    ::HIR::Struct {
                        LowerHIR_GenericParams(ent.params(), nullptr),
                        ::HIR::Struct::Repr::Rust,
                        mv$(data)
                        }
                    );
                auto ty_ipath = path;
                ty_ipath.name = ty_name.c_str();
                auto ty_path = ty_ipath.get_full_path();
                // Add type params
                {
                    auto& params = ty_path.m_data.as_Generic().m_params;
                    unsigned int i = 0;
                    for(const auto& typ : ent.params().ty_params())
                        params.m_types.push_back( ::HIR::TypeRef/*::new_generic*/(typ.name(), i++) );
                }
                variants.push_back({ var.m_name, var.m_data.is_Struct(), ::HIR::TypeRef::new_path( mv$(ty_path), {} ) });
            }
        }

        if( /*const auto* attr_repr =*/ attrs.get("repr") )
        {
            // NOTE: librustc_llvm has `#[repr(C)] enum AttributePlace { Argument(u32), Function }`
            //ERROR(Span(), E0000, "#[repr] not allowed on enums with data");
        }
        data = ::HIR::Enum::Class::make_Data( mv$(variants) );
    }

    return ::HIR::Enum {
        LowerHIR_GenericParams(ent.params(), nullptr),
        mv$(data)
        };
}
::HIR::Union LowerHIR_Union(::HIR::ItemPath path, const ::AST::Union& f, const ::AST::AttributeList& attrs)
{
    auto priv_path = ::HIR::Publicity::new_priv( get_parent_module(path) );
    auto get_pub = [&](bool is_pub){ return is_pub ? ::HIR::Publicity::new_global() : priv_path; };

    auto repr = ::HIR::Union::Repr::Rust;

    if( const auto* attr_repr = attrs.get("repr") )
    {
        ASSERT_BUG(Span(), attr_repr->has_sub_items(), "#[repr] attribute malformed, " << *attr_repr);
        ASSERT_BUG(Span(), attr_repr->items().size() == 1, "#[repr] attribute malformed, " << *attr_repr);
        ASSERT_BUG(Span(), attr_repr->items()[0].has_noarg(), "#[repr] attribute malformed, " << *attr_repr);
        const auto& repr_str = attr_repr->items()[0].name();
        if( repr_str == "C" ) {
            repr = ::HIR::Union::Repr::C;
        }
        else {
            ERROR(attr_repr->span(), E0000, "Unknown union repr '" << repr_str << "'");
        }
    }

    ::HIR::Struct::Data::Data_Named variants;
    for(const auto& field : f.m_variants)
        variants.push_back( ::std::make_pair( field.m_name, new_visent(get_pub(field.m_is_public), LowerHIR_Type(field.m_type)) ) );

    return ::HIR::Union {
        LowerHIR_GenericParams(f.m_params, nullptr),
        repr,
        mv$(variants)
        };
}
::HIR::Trait LowerHIR_Trait(::HIR::SimplePath trait_path, const ::AST::Trait& f)
{
    TRACE_FUNCTION_F(trait_path);
    trait_path.m_crate_name = g_crate_name;

    bool trait_reqires_sized = false;
    auto params = LowerHIR_GenericParams(f.params(), &trait_reqires_sized);

    ::HIR::LifetimeRef  lifetime;
    ::std::vector< ::HIR::TraitPath>    supertraits;
    for(const auto& st : f.supertraits()) {
        if( st.ent.path.is_valid() ) {
            supertraits.push_back( LowerHIR_TraitPath(st.sp, st.ent.path) );
        }
        else {
            lifetime = ::HIR::LifetimeRef::new_static();
        }
    }
    ::HIR::Trait    rv {
        mv$(params),
        mv$(lifetime),
        mv$(supertraits)
        };

    {
        auto this_trait = ::HIR::GenericPath( trait_path );
        unsigned int i = 0;
        for(const auto& arg : rv.m_params.m_types) {
            this_trait.m_params.m_types.push_back( ::HIR::TypeRef(arg.m_name, i) );
            i ++;
        }
        // HACK: Add a bound of Self: ThisTrait for parts of typeck (TODO: Remove this, it's evil)
        rv.m_params.m_bounds.push_back( ::HIR::GenericBound::make_TraitBound({ ::HIR::TypeRef("Self",0xFFFF), { mv$(this_trait) } }) );
    }

    for(const auto& item : f.items())
    {
        auto trait_ip = ::HIR::ItemPath(trait_path);
        auto item_path = ::HIR::ItemPath( trait_ip, item.name.c_str() );

        TU_MATCH_DEF(::AST::Item, (item.data), (i),
        (
            BUG(item.span, "Encountered unexpected item type in trait");
            ),
        (None,
            // Ignore.
            ),
        (MacroInv,
            // Ignore.
            ),
        (Type,
            bool is_sized = true;
            ::std::vector< ::HIR::TraitPath>    trait_bounds;
            ::HIR::LifetimeRef  lifetime_bound;
            auto gps = LowerHIR_GenericParams(i.params(), &is_sized);

            for(auto& b : gps.m_bounds)
            {
                TU_MATCH(::HIR::GenericBound, (b), (be),
                (TypeLifetime,
                    ASSERT_BUG(item.span, be.type == ::HIR::TypeRef("Self", 0xFFFF), "Invalid lifetime bound on associated type");
                    lifetime_bound = mv$(be.valid_for);
                    ),
                (TraitBound,
                    ASSERT_BUG(item.span, be.type == ::HIR::TypeRef("Self", 0xFFFF), "Invalid trait bound on associated type");
                    trait_bounds.push_back( mv$(be.trait) );
                    ),
                (Lifetime,
                    BUG(item.span, "Unexpected lifetime-lifetime bound on associated type");
                    ),
                (TypeEquality,
                    BUG(item.span, "Unexpected type equality bound on associated type");
                    )
                )
            }
            rv.m_types.insert( ::std::make_pair(item.name, ::HIR::AssociatedType {
                is_sized,
                mv$(lifetime_bound),
                mv$(trait_bounds),
                LowerHIR_Type(i.type())
                }) );
            ),
        (Function,
            ::HIR::TypeRef  self_type {"Self", 0xFFFF};
            auto fcn = LowerHIR_Function(item_path, item.attrs, i, self_type);
            fcn.m_save_code = true;
            rv.m_values.insert( ::std::make_pair(item.name, ::HIR::TraitValueItem::make_Function( mv$(fcn) )) );
            ),
        (Static,
            if( i.s_class() == ::AST::Static::CONST )
                rv.m_values.insert( ::std::make_pair(item.name, ::HIR::TraitValueItem::make_Constant(::HIR::Constant {
                    ::HIR::GenericParams {},
                    LowerHIR_Type( i.type() ),
                    LowerHIR_Expr( i.value() )
                    })) );
            else {
                ::HIR::Linkage  linkage;
                rv.m_values.insert( ::std::make_pair(item.name, ::HIR::TraitValueItem::make_Static(::HIR::Static {
                    mv$(linkage),
                    (i.s_class() == ::AST::Static::MUT),
                    LowerHIR_Type( i.type() ),
                    LowerHIR_Expr( i.value() )
                    })) );
            }
            )
        )
    }

    rv.m_is_marker = f.is_marker();

    return rv;
}
::HIR::Function LowerHIR_Function(::HIR::ItemPath p, const ::AST::AttributeList& attrs, const ::AST::Function& f, const ::HIR::TypeRef& real_self_type)
{
    static Span sp;
    static HIR::TypeRef explicit_self_type = HIR::TypeRef("Self", 0xFFFF);

    TRACE_FUNCTION_F(p);

    ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef > >    args;
    for(const auto& arg : f.args())
        args.push_back( ::std::make_pair( LowerHIR_Pattern(arg.first), LowerHIR_Type(arg.second) ) );

    auto receiver = ::HIR::Function::Receiver::Free;

    if( args.size() > 0 && args.front().first.m_binding.m_name == "self" )
    {
        const auto& arg_self_ty = args.front().second;
        if( arg_self_ty == explicit_self_type || arg_self_ty == real_self_type ) {
            receiver = ::HIR::Function::Receiver::Value;
        }
        else TU_IFLET(::HIR::TypeRef::Data, arg_self_ty.m_data, Borrow, e,
            if( *e.inner == explicit_self_type || *e.inner == real_self_type )
            {
                switch(e.type)
                {
                case ::HIR::BorrowType::Owned:  receiver = ::HIR::Function::Receiver::BorrowOwned;  break;
                case ::HIR::BorrowType::Unique: receiver = ::HIR::Function::Receiver::BorrowUnique; break;
                case ::HIR::BorrowType::Shared: receiver = ::HIR::Function::Receiver::BorrowShared; break;
                }
            }
        )
        else TU_IFLET(::HIR::TypeRef::Data, arg_self_ty.m_data, Path, e,
            // Box - Compare with `owned_box` lang item
            TU_IFLET(::HIR::Path::Data, e.path.m_data, Generic, pe,
                auto p = g_crate_ptr->get_lang_item_path_opt("owned_box");
                if( pe.m_path == p )
                {
                    if( pe.m_params.m_types.size() == 1 && (pe.m_params.m_types[0] == explicit_self_type || pe.m_params.m_types[0] == real_self_type) )
                    {
                        receiver = ::HIR::Function::Receiver::Box;
                    }
                }
                // TODO: for other types, support arbitary structs/paths.
                // - The path must include Self as a (the only?) type param.
                if( receiver == ::HIR::Function::Receiver::Free )
                {
                    if( pe.m_params.m_types.size() == 0 ) {
                        ERROR(sp, E0000, "Unsupported receiver type - " << arg_self_ty);
                    }
                    if( pe.m_params.m_types.size() != 1 ) {
                        TODO(sp, "Receiver types with more than one param - " << arg_self_ty);
                    }
                    if( !(pe.m_params.m_types[0] == explicit_self_type || pe.m_params.m_types[0] == real_self_type) ) {
                        ERROR(sp, E0000, "Unsupported receiver type - " << arg_self_ty);
                    }
                    receiver = ::HIR::Function::Receiver::Custom;
                }
            )
        )
        else {
        }

        if( receiver == ::HIR::Function::Receiver::Free )
        {
            ERROR(sp, E0000, "Unknown receiver type - " << arg_self_ty);
        }
    }

    bool force_emit = false;
    if( const auto* a = attrs.get("inline") )
    {
        if( a->has_sub_items() && ::std::any_of(a->items().begin(), a->items().end(), [](const auto& v){ return v.name() == "never"; }) ) {
            // Inline(never)
        }
        else {
            force_emit = true;
        }
    }

    ::HIR::Linkage  linkage;

    // Convert #[link_name/no_mangle] attributes into the name
    if( g_ast_crate_ptr->m_test_harness && f.code().is_valid() )
    {
        // If we're making a test harness, and this item defines code, don't apply the linkage rules
    }
    else if( const auto* a = attrs.get("link_name") )
    {
        if( !a->has_string() )
            ERROR(sp, E0000, "#[link_name] requires a string");
        linkage.name = a->string();
    }
    else if( const auto* a = attrs.get("no_mangle") )
    {
        (void)a;
        linkage.name = p.get_name();
    }
    else if( const auto* a = attrs.get("lang") )
    {
        if( a->string() == "panic_fmt")
        {
            linkage.name = "rust_begin_unwind";
        }
    }
    else
    {
        // Leave linkage.name as empty
    }

    // If there's no code, mangle the name (According to the ABI) and set linkage.
    if( linkage.name == "" && ! f.code().is_valid() )
    {
        linkage.name = p.get_name();
    }

    return ::HIR::Function {
        force_emit,
        mv$(linkage),
        receiver,
        f.abi(), f.is_unsafe(), f.is_const(),
        LowerHIR_GenericParams(f.params(), nullptr),    // TODO: If this is a method, then it can add the Self: Sized bound
        mv$(args), f.is_variadic(),
        LowerHIR_Type( f.rettype() ),
        LowerHIR_Expr( f.code() )
        };
}

void _add_mod_ns_item(::HIR::Module& mod, RcString name, ::HIR::Publicity is_pub,  ::HIR::TypeItem ti) {
    mod.m_mod_items.insert( ::std::make_pair( mv$(name), ::make_unique_ptr(::HIR::VisEnt< ::HIR::TypeItem> { is_pub, mv$(ti) }) ) );
}
void _add_mod_val_item(::HIR::Module& mod, RcString name, ::HIR::Publicity is_pub,  ::HIR::ValueItem ti) {
    mod.m_value_items.insert( ::std::make_pair( mv$(name), ::make_unique_ptr(::HIR::VisEnt< ::HIR::ValueItem> { is_pub, mv$(ti) }) ) );
}
void _add_mod_mac_item(::HIR::Module& mod, RcString name, ::HIR::Publicity is_pub,  ::HIR::MacroItem ti) {
    mod.m_macro_items.insert( ::std::make_pair( mv$(name), ::make_unique_ptr(::HIR::VisEnt< ::HIR::MacroItem> { is_pub, mv$(ti) }) ) );
}

::HIR::Module LowerHIR_Module(const ::AST::Module& ast_mod, ::HIR::ItemPath path, ::std::vector< ::HIR::SimplePath> traits)
{
    TRACE_FUNCTION_F("path = " << path);
    ::HIR::Module   mod { };

    mod.m_traits = mv$(traits);

    auto priv_path = ::HIR::Publicity::new_priv( path.get_simple_path() );
    auto get_pub = [&](bool is_pub)->::HIR::Publicity{ return (is_pub ? ::HIR::Publicity::new_global() : priv_path); };

    // Populate trait list
    for(const auto& item : ast_mod.m_type_items)
    {
        if( item.second.path.m_bindings.type.is_Trait() ) {
            auto sp = LowerHIR_SimplePath(Span(), item.second.path);
            if( ::std::find(mod.m_traits.begin(), mod.m_traits.end(), sp) == mod.m_traits.end() )
                mod.m_traits.push_back( mv$(sp) );
        }
    }

    for( unsigned int i = 0; i < ast_mod.anon_mods().size(); i ++ )
    {
        const auto& submod_ptr = ast_mod.anon_mods()[i];
        if( submod_ptr )
        {
            auto& submod = *submod_ptr;
            auto name = RcString::new_interned(FMT("#" << i));
            auto item_path = ::HIR::ItemPath(path, name.c_str());
            auto ti = ::HIR::TypeItem::make_Module( LowerHIR_Module(submod, item_path, mod.m_traits) );
            _add_mod_ns_item( mod,  mv$(name), get_pub(false), mv$(ti) );
        }
    }

    for( const auto& item : ast_mod.items() )
    {
        const auto& sp = item.span;
        auto item_path = ::HIR::ItemPath(path, item.name.c_str());
        DEBUG(item_path << " " << item.data.tag_str());
        TU_MATCH(::AST::Item, (item.data), (e),
        (None,
            ),
        (Macro,
            ),
        (MacroInv,
            // Valid.
            //BUG(sp, "Stray macro invocation in " << path);
            ),
        (ExternBlock,
            if( e.items().size() > 0 )
            {
                TODO(sp, "Expand ExternBlock");
            }
            // Insert a record of the `link` attribute
            for(const auto& a : item.attrs.m_items)
            {
                if( a.name() != "link" )    continue ;

                ::std::string   name;
                for(const auto& i : a.items())
                {
                    if( i.name() == "name" ) {
                        name = i.string();
                    }
                    else {
                    }
                }
                if( name != "" )
                {
                    g_crate_ptr->m_ext_libs.push_back( ::HIR::ExternLibrary { name } );
                }
                else {
                    ERROR(sp, E0000, "#[link] needs `name`");
                }
            }
            ),
        (Impl,
            // NOTE: impl blocks are handled in a second pass
            ),
        (NegImpl,
            // NOTE: impl blocks are handled in a second pass
            ),
        (Use,
            // Ignore - The index is used to add `Import`s
            ),
        (Module,
            _add_mod_ns_item( mod, item.name, get_pub(item.is_pub), LowerHIR_Module(e, mv$(item_path)) );
            ),
        (Crate,
            // All 'extern crate' items should be normalised into a list in the crate root
            // - If public, add a namespace import here referring to the root of the imported crate
            _add_mod_ns_item( mod, item.name, get_pub(item.is_pub), ::HIR::TypeItem::make_Import({ ::HIR::SimplePath(e.name, {}), false, 0} ) );
            ),
        (Type,
            if( e.type().m_data.is_Any() )
            {
                if( !e.params().lft_params().empty() || !e.params().ty_params().empty() || !e.params().bounds().empty() )
                {
                    ERROR(item.span, E0000, "Generics on extern type");
                }
                _add_mod_ns_item(mod, item.name, get_pub(item.is_pub), ::HIR::ExternType {});
                break;
            }
            _add_mod_ns_item( mod,  item.name, get_pub(item.is_pub), ::HIR::TypeItem::make_TypeAlias( LowerHIR_TypeAlias(e) ) );
            ),
        (Struct,
            /// Add value reference
            if( e.m_data.is_Unit() ) {
                _add_mod_val_item( mod,  item.name, get_pub(item.is_pub), ::HIR::ValueItem::make_StructConstant({item_path.get_simple_path()}) );
            }
            else if( e.m_data.is_Tuple() ) {
                _add_mod_val_item( mod,  item.name, get_pub(item.is_pub), ::HIR::ValueItem::make_StructConstructor({item_path.get_simple_path()}) );
            }
            else {
            }
            _add_mod_ns_item( mod,  item.name, get_pub(item.is_pub), LowerHIR_Struct(item_path, e, item.attrs) );
            ),
        (Enum,
            auto enm = LowerHIR_Enum(item_path, e, item.attrs, [&](auto name, auto str){ _add_mod_ns_item(mod, name, get_pub(item.is_pub), mv$(str)); });
            _add_mod_ns_item( mod,  item.name, get_pub(item.is_pub), mv$(enm) );
            ),
        (Union,
            _add_mod_ns_item( mod,  item.name, get_pub(item.is_pub), LowerHIR_Union(item_path, e, item.attrs) );
            ),
        (Trait,
            _add_mod_ns_item( mod,  item.name, get_pub(item.is_pub), LowerHIR_Trait(item_path.get_simple_path(), e) );
            ),
        (Function,
            _add_mod_val_item(mod, item.name, get_pub(item.is_pub),  LowerHIR_Function(item_path, item.attrs, e, ::HIR::TypeRef{}));
            ),
        (Static,
            if( e.s_class() == ::AST::Static::CONST )
                _add_mod_val_item(mod, item.name, get_pub(item.is_pub),  ::HIR::ValueItem::make_Constant(::HIR::Constant {
                    ::HIR::GenericParams {},
                    LowerHIR_Type( e.type() ),
                    LowerHIR_Expr( e.value() )
                    }));
            else {
                ::HIR::Linkage  linkage;

                // If there's no code, demangle the name (TODO: By ABI) and set linkage.
                if( linkage.name == "" && ! e.value().is_valid() )
                {
                    linkage.name = item.name.c_str();
                }

                _add_mod_val_item(mod, item.name, get_pub(item.is_pub),  ::HIR::ValueItem::make_Static(::HIR::Static {
                    mv$(linkage),
                    (e.s_class() == ::AST::Static::MUT),
                    LowerHIR_Type( e.type() ),
                    LowerHIR_Expr( e.value() )
                    }));
            }
            )
        )
    }

    Span    mod_span;
    for( const auto& ie : ast_mod.m_namespace_items )
    {
        const auto& sp = mod_span;
        if( ie.second.is_import ) {
            auto hir_path = LowerHIR_SimplePath( sp, ie.second.path );
            ::HIR::TypeItem ti;
            if( const auto* pb = ie.second.path.m_bindings.type.opt_EnumVar() ) {
                DEBUG("Import NS " << ie.first << " = " << hir_path << " (Enum Variant)");
                ti = ::HIR::TypeItem::make_Import({ mv$(hir_path), true, pb->idx });
            }
            else {
                DEBUG("Import NS " << ie.first << " = " << hir_path);
                ti = ::HIR::TypeItem::make_Import({ mv$(hir_path), false, 0 });
            }
            _add_mod_ns_item(mod, ie.first, get_pub(ie.second.is_pub), mv$(ti));
        }
    }
    for( const auto& ie : ast_mod.m_value_items )
    {
        const auto& sp = mod_span;
        if( ie.second.is_import ) {
            auto hir_path = LowerHIR_SimplePath( sp, ie.second.path );
            ::HIR::ValueItem    vi;

            TU_MATCH_HDRA( (ie.second.path.m_bindings.value), {)
            default:
                DEBUG("Import VAL " << ie.first << " = " << hir_path);
                vi = ::HIR::ValueItem::make_Import({ mv$(hir_path), false, 0 });
            TU_ARMA(EnumVar, pb) {
                DEBUG("Import VAL " << ie.first << " = " << hir_path << " (Enum Variant)");
                vi = ::HIR::ValueItem::make_Import({ mv$(hir_path), true, pb.idx });
                }
            }
            _add_mod_val_item(mod, ie.first, get_pub(ie.second.is_pub), mv$(vi));
        }
    }

    for( const auto& ie : ast_mod.m_macro_imports )
    {
        //const auto& sp = mod_span;
        if( ie.is_pub )
        {
            auto mi = ::HIR::MacroItem::make_Import({ ::HIR::SimplePath(ie.path.front(), ::std::vector<RcString>(ie.path.begin()+1, ie.path.end())) });
            _add_mod_mac_item( mod, ie.name, get_pub(true), mv$(mi) );
        }
    }

    return mod;
}

void LowerHIR_Module_Impls(const ::AST::Module& ast_mod,  ::HIR::Crate& hir_crate)
{
    DEBUG(ast_mod.path());

    // Sub-modules
    for( const auto& item : ast_mod.items() )
    {
        TU_IFLET(::AST::Item, item.data, Module, e,
            LowerHIR_Module_Impls(e,  hir_crate);
        )
    }
    for( const auto& submod_ptr : ast_mod.anon_mods() )
    {
        if( submod_ptr ) {
            LowerHIR_Module_Impls(*submod_ptr,  hir_crate);
        }
    }

    //
    for( const auto& i : ast_mod.items() )
    {
        if( !i.data.is_Impl() ) continue;
        const auto& impl = i.data.as_Impl();
        const Span  impl_span;
        auto params = LowerHIR_GenericParams(impl.def().params(), nullptr);

        TRACE_FUNCTION_F("IMPL " << impl.def());

        if( impl.def().trait().ent.is_valid() )
        {
            const auto& pb = impl.def().trait().ent.m_bindings.type;
            ASSERT_BUG(Span(), pb.is_Trait(), "Binding for trait path in impl isn't a Trait - " << impl.def().trait().ent);
            ASSERT_BUG(Span(), pb.as_Trait().trait_ || pb.as_Trait().hir, "Trait pointer for trait path in impl isn't set");
            bool is_marker = (pb.as_Trait().trait_ ? pb.as_Trait().trait_->is_marker() : pb.as_Trait().hir->m_is_marker);
            auto trait_path = LowerHIR_GenericPath(impl.def().trait().sp, impl.def().trait().ent);
            auto trait_name = mv$(trait_path.m_path);
            auto trait_args = mv$(trait_path.m_params);

            if( !is_marker )
            {
                auto type = LowerHIR_Type(impl.def().type());

                ::HIR::ItemPath    path(type, trait_name, trait_args);
                DEBUG(path);

                ::std::map< RcString, ::HIR::TraitImpl::ImplEnt< ::HIR::Function> > methods;
                ::std::map< RcString, ::HIR::TraitImpl::ImplEnt< ::HIR::Constant> > constants;
                ::std::map< RcString, ::HIR::TraitImpl::ImplEnt< ::HIR::TypeRef> > types;

                for(const auto& item : impl.items())
                {
                    ::HIR::ItemPath    item_path(path, item.name.c_str());
                    TU_MATCH_DEF(::AST::Item, (*item.data), (e),
                    (
                        BUG(item.sp, "Unexpected item type in trait impl - " << item.data->tag_str());
                        ),
                    (None,
                        ),
                    (MacroInv,
                        ),
                    (Static,
                        if( e.s_class() == ::AST::Static::CONST ) {
                            // TODO: Check signature against the trait?
                            constants.insert( ::std::make_pair(item.name, ::HIR::TraitImpl::ImplEnt< ::HIR::Constant> { item.is_specialisable, ::HIR::Constant {
                                ::HIR::GenericParams {},
                                LowerHIR_Type( e.type() ),
                                LowerHIR_Expr( e.value() )
                                } }) );
                        }
                        else {
                            TODO(item.sp, "Associated statics in trait impl");
                        }
                        ),
                    (Type,
                        DEBUG("- type " << item.name);
                        types.insert( ::std::make_pair(item.name, ::HIR::TraitImpl::ImplEnt< ::HIR::TypeRef> { item.is_specialisable, LowerHIR_Type(e.type()) }) );
                        ),
                    (Function,
                        DEBUG("- method " << item.name);
                        methods.insert( ::std::make_pair(item.name, ::HIR::TraitImpl::ImplEnt< ::HIR::Function> { item.is_specialisable, LowerHIR_Function(item_path, item.attrs, e, type) }) );
                        )
                    )
                }

                // Sorted later on
                hir_crate.m_trait_impls[mv$(trait_name)].generic.push_back(box$(::HIR::TraitImpl {
                    mv$(params),
                    mv$(trait_args),
                    mv$(type),

                    mv$(methods),
                    mv$(constants),
                    {}, // Statics
                    mv$(types),

                    LowerHIR_SimplePath(Span(), ast_mod.path())
                    }));
            }
            else if( impl.def().type().m_data.is_None() )
            {
                // Ignore - These are encoded in the 'is_marker' field of the trait
            }
            else
            {
                auto type = LowerHIR_Type(impl.def().type());
                hir_crate.m_marker_impls[mv$(trait_name)].generic.push_back(box$(::HIR::MarkerImpl {
                    mv$(params),
                    mv$(trait_args),
                    true,
                    mv$(type),

                    LowerHIR_SimplePath(Span(), ast_mod.path())
                    }));
            }
        }
        else
        {
            // Inherent impls
            auto type = LowerHIR_Type(impl.def().type());
            ::HIR::ItemPath    path(type);

            auto priv_path = ::HIR::Publicity::new_priv( LowerHIR_SimplePath(Span(), ast_mod.path()) ); // TODO: Does this need to consume anon modules?
            auto get_pub = [&](bool is_pub){ return is_pub ? ::HIR::Publicity::new_global() : priv_path; };

            ::std::map< RcString, ::HIR::TypeImpl::VisImplEnt< ::HIR::Function> > methods;
            ::std::map< RcString, ::HIR::TypeImpl::VisImplEnt< ::HIR::Constant> > constants;

            for(const auto& item : impl.items())
            {
                ::HIR::ItemPath    item_path(path, item.name.c_str());
                TU_MATCH_DEF(::AST::Item, (*item.data), (e),
                (
                    BUG(item.sp, "Unexpected item type in inherent impl - " << item.data->tag_str());
                    ),
                (None,
                    ),
                (MacroInv,
                    ),
                (Static,
                    if( e.s_class() == ::AST::Static::CONST ) {
                        constants.insert( ::std::make_pair(item.name, ::HIR::TypeImpl::VisImplEnt< ::HIR::Constant> { get_pub(item.is_pub), item.is_specialisable, ::HIR::Constant {
                            ::HIR::GenericParams {},
                            LowerHIR_Type( e.type() ),
                            LowerHIR_Expr( e.value() )
                            } }) );
                    }
                    else {
                        TODO(item.sp, "Associated statics in inherent impl");
                    }
                    ),
                (Function,
                    methods.insert( ::std::make_pair(item.name, ::HIR::TypeImpl::VisImplEnt< ::HIR::Function> {
                        get_pub(item.is_pub), item.is_specialisable, LowerHIR_Function(item_path, item.attrs, e, type)
                        } ) );
                    )
                )
            }

            // Sorted later on
            hir_crate.m_type_impls.generic.push_back( box$(::HIR::TypeImpl {
                mv$(params),
                mv$(type),
                mv$(methods),
                mv$(constants),

                LowerHIR_SimplePath(Span(), ast_mod.path())
                }) );
        }
    }
    for( const auto& i : ast_mod.items() )
    {
        if( !i.data.is_NegImpl() ) continue;
        const auto& impl = i.data.as_NegImpl();

        auto params = LowerHIR_GenericParams(impl.params(), nullptr);
        auto type = LowerHIR_Type(impl.type());
        auto trait = LowerHIR_GenericPath(impl.trait().sp, impl.trait().ent);
        auto trait_name = mv$(trait.m_path);
        auto trait_args = mv$(trait.m_params);

        // Sorting done later
        hir_crate.m_marker_impls[mv$(trait_name)].generic.push_back(box$(::HIR::MarkerImpl {
            mv$(params),
            mv$(trait_args),
            false,
            mv$(type),

            LowerHIR_SimplePath(Span(), ast_mod.path())
            }) );
    }
}


class IndexVisitor:
    public ::HIR::Visitor
{
    const ::HIR::Crate& crate;
    Span    null_span;
public:
    IndexVisitor(const ::HIR::Crate& crate):
        crate(crate)
    {}

    void visit_params(::HIR::GenericParams& params) override
    {
        for( auto& bound : params.m_bounds )
        {
            TU_IFLET(::HIR::GenericBound, bound, TraitBound, e,
                e.trait.m_trait_ptr = &this->crate.get_trait_by_path(null_span, e.trait.m_path.m_path);
            )
        }
    }
};

/// \brief Converts the AST into HIR format
///
/// - Removes all possibility for unexpanded macros
/// - Performs desugaring of for/if-let/while-let/...
::HIR::CratePtr LowerHIR_FromAST(::AST::Crate crate)
{
    ::HIR::Crate    rv;

    if(crate.m_crate_type != ::AST::Crate::Type::Executable)
    {
        if(crate.m_crate_name_suffix != "")
        {
            rv.m_crate_name = RcString::new_interned(FMT(crate.m_crate_name + "-" + crate.m_crate_name_suffix));
        }
        else
        {
            rv.m_crate_name = RcString::new_interned(crate.m_crate_name);
        }
    }

    g_crate_ptr = &rv;
    g_ast_crate_ptr = &crate;
    g_crate_name = rv.m_crate_name;
    g_core_crate = (crate.m_load_std == ::AST::Crate::LOAD_NONE ? rv.m_crate_name : RcString::new_interned("core"));
    auto& macros = rv.m_exported_macros;

    // - Extract exported macros
    {
        ::std::vector< ::AST::Module*>    mods;
        mods.push_back( &crate.m_root_module );
        do
        {
            auto& mod = *mods.back();
            mods.pop_back();

            for( /*const*/ auto& mac : mod.macros() ) {
                if( mac.data->m_exported ) {
                    auto res = macros.insert( ::std::make_pair( mac.name, mv$(mac.data) ) );
                    if( res.second )
                        DEBUG("- Define " << mac.name << "!");
                }
                else {
                    DEBUG("- Non-exported " << mac.name << "!");
                }
            }

            for(auto& i : mod.items()) {
                if( i.data.is_Module() )
                    mods.push_back( &i.data.as_Module() );
            }
        } while( mods.size() > 0 );

        for( auto& mac : crate.m_root_module.macro_imports_res() ) {
            if( mac.data->m_exported && mac.name != "" ) {
                auto v = ::std::make_pair( mac.name, MacroRulesPtr(new MacroRules( mv$(*const_cast<MacroRules*>(mac.data)) )) );
                auto it = macros.find(mac.name);
                if( it == macros.end() )
                {
                    auto res = macros.insert( mv$(v) );
                    DEBUG("- Import " << mac.name << "! (from \"" << res.first->second->m_source_crate << "\")");
                }
                else if( v.second->m_rules.empty() ) {
                    // Skip
                }
                else {
                    DEBUG("- Replace " << mac.name << "! (from \"" << it->second->m_source_crate << "\") with one from \"" << v.second->m_source_crate << "\"");
                    it->second = mv$( v.second );
                }
            }
        }
        for( const auto& mac : crate.m_root_module.m_macro_imports )
        {
            if( mac.is_pub )
            {
                if( !mac.macro_ptr ) {
                    // Add to the re-export list
                    auto path = ::HIR::SimplePath(mac.path.front(), ::std::vector<RcString>(mac.path.begin()+1, mac.path.end()));;
                    rv.m_proc_macro_reexports.insert( ::std::make_pair( mac.name, ::HIR::Crate::MacroImport { path } ));
                    continue ;
                }
                // TODO: Why does this to such a move?
                auto v = ::std::make_pair( mac.name, MacroRulesPtr(new MacroRules( mv$(*const_cast<MacroRules*>(mac.macro_ptr)) )) );

                auto it = macros.find(mac.name);
                if( it == macros.end() )
                {
                    auto res = macros.insert( mv$(v) );
                    DEBUG("- Import " << mac.name << "! (from \"" << res.first->second->m_source_crate << "\")");
                }
                else if( v.second->m_rules.empty() ) {
                    // Skip
                }
                else {
                    DEBUG("- Replace " << mac.name << "! (from \"" << it->second->m_source_crate << "\") with one from \"" << v.second->m_source_crate << "\"");
                    it->second = mv$( v.second );
                }
            }
        }
    }
    // - Proc Macros
    if( crate.m_crate_type == ::AST::Crate::Type::ProcMacro )
    {
        for(const auto& ent : crate.m_proc_macros)
        {
            // Register under an invalid simplepath
            rv.m_proc_macros.push_back( ::HIR::ProcMacro { ent.name, ::HIR::SimplePath(RcString(""), { ent.name }), ent.attributes } );
        }
    }
    else
    {
        ASSERT_BUG(Span(), crate.m_proc_macros.size() == 0, "Procedural macros defined in non proc-macro crate");
    }

    auto sp = Span();
    // - Store the lang item paths so conversion code can use them.
    for( const auto& lang_item_path : crate.m_lang_items )
    {
        rv.m_lang_items.insert( ::std::make_pair(lang_item_path.first, LowerHIR_SimplePath(sp, lang_item_path.second)) );
    }
    for(auto& ext_crate : crate.m_extern_crates)
    {
        // Populate m_lang_items from loaded crates too
        for( const auto& lang : ext_crate.second.m_hir->m_lang_items )
        {
            const auto& name = lang.first;
            const auto& path = lang.second;
            auto irv = rv.m_lang_items.insert( ::std::make_pair(name, path) );
            if( irv.second == true )
            {
                // Doesn't yet exist, all good
            }
            else if( irv.first->second == path )
            {
                // Equal definitions, also good (TODO: How can this happen?)
            }
            else if( irv.first->second.m_components.empty() && path.m_components.empty() )
            {
                // Both are just markers, also good (e.g. #![needs_panic_runtime])
            }
            else
            {
                ERROR(sp, E0000, "Conflicting definitions of lang item '" << name << "'. " << path << " and " << irv.first->second);
            }
        }
        auto p1 = ext_crate.second.m_filename.rfind('/');
        auto p2 = ext_crate.second.m_filename.rfind('\\');
        auto p = (p1 == ::std::string::npos ? p2 : (p2 == ::std::string::npos ? p1 : ::std::max(p1,p2)));
        auto crate_file = (p == ::std::string::npos ? ext_crate.second.m_filename : ext_crate.second.m_filename.substr(p+1));
        rv.m_ext_crates.insert( ::std::make_pair( ext_crate.first, ::HIR::ExternCrate { mv$(ext_crate.second.m_hir), crate_file, ext_crate.second.m_filename } ) );
    }
    path_Sized = rv.get_lang_item_path(sp, "sized");

    rv.m_root_module = LowerHIR_Module( crate.m_root_module, ::HIR::ItemPath(rv.m_crate_name) );

    LowerHIR_Module_Impls(crate.m_root_module,  rv);

    // Set all pointers in the HIR to the correct (now fixed) locations
    IndexVisitor(rv).visit_crate( rv );

    // HACK: If the current crate is libcore, store the paths to various non-lang ops items
    // - Some operators aren't tagged with #[lang], so this works around that
    if( crate.m_crate_name == "core" )
    {
        struct H {
            static ::HIR::SimplePath resolve_path(const ::HIR::Crate& crate, bool is_value, ::std::initializer_list<const char*> n)
            {
                ::HIR::SimplePath   cur_path("", {});

                const ::HIR::Module* mod = &crate.m_root_module;
                assert(n.begin() != n.end());
                for(auto it = n.begin(); it != n.end()-1; ++it)
                {
                    auto it2 = mod->m_mod_items.find(*it);
                    if( it2 == mod->m_mod_items.end() )
                        return ::HIR::SimplePath();
                    const auto& e = it2->second;
                    if(const auto* ip = e->ent.opt_Import())
                    {
                        // TODO: Handle module aliases?
                        (void)ip;
                        return ::HIR::SimplePath();
                    }
                    else if(const auto* ep = e->ent.opt_Module() )
                    {
                        cur_path.m_components.push_back(*it);
                        mod = ep;
                    }
                    else
                    {
                        // Incorrect item type
                        return ::HIR::SimplePath();
                    }
                }

                auto last = *(n.end()-1);
                if( is_value )
                {
                    throw "";
                }
                else
                {
                    auto it2 = mod->m_mod_items.find(last);
                    if( it2 == mod->m_mod_items.end() )
                        return ::HIR::SimplePath();

                    // Found: Either return the current path, or return this alias.
                    if(const auto* ip = it2->second->ent.opt_Import())
                    {
                        if(ip->is_variant)
                            return ::HIR::SimplePath();
                        return ip->path;
                    }
                    else
                    {
                        cur_path.m_components.push_back(last);
                        return cur_path;
                    }
                }
            }
        };
        // Check for existing defintions of lang items before adding magic ones
        if( TARGETVER_1_19 )
        {
            if( rv.m_lang_items.count("boxed_trait") == 0 )
            {
                rv.m_lang_items.insert(::std::make_pair( ::std::string("boxed_trait"),  H::resolve_path(rv, false, {"ops", "Boxed"}) ));
            }
            if( rv.m_lang_items.count("placer_trait") == 0 )
            {
                rv.m_lang_items.insert(::std::make_pair( ::std::string("placer_trait"),  H::resolve_path(rv, false, {"ops", "Placer"}) ));
            }
            if( rv.m_lang_items.count("place_trait") == 0 )
            {
                rv.m_lang_items.insert(::std::make_pair( ::std::string("place_trait"),  H::resolve_path(rv, false, {"ops", "Place"}) ));
            }
            if( rv.m_lang_items.count("box_place_trait") == 0 )
            {
                rv.m_lang_items.insert(::std::make_pair( ::std::string("box_place_trait"),  H::resolve_path(rv, false, {"ops", "BoxPlace"}) ));
            }
            if( rv.m_lang_items.count("in_place_trait") == 0 )
            {
                rv.m_lang_items.insert(::std::make_pair( ::std::string("in_place_trait"),  H::resolve_path(rv, false, {"ops", "InPlace"}) ));
            }
        }
    }

    g_crate_ptr = nullptr;
    return ::HIR::CratePtr( mv$(rv) );
}


