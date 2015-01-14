#include "nailtool.h"
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/foreach.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <list>

/* XXX: We need to get rid of NailStream. Find some way of statically determining which NailStream
   some transform will create and inline calls to that.
   Then implement a coroutine based parser */
class CDirectParser {
  std::ostream &final;
  std::ostream &hdr;
  std::stringstream out,header;
  int num_iters;
  int nr_dep;
  std::string arena; // Which arena to allocate from (local variable name)
  Endian End;

  void check_int(unsigned int width, const std::string &fail){
    if(option::templates){
     out << "if(parser_fail(str_current.check("<<width<<"))){ "<<fail<<"}\n";
    } else {
      out << "if(parser_fail(stream_check(str_current,"<<width<<"))) {"<<fail<<"}\n";
    }
  }
  std::string int_expr(const char * stream,unsigned int width){
    if(width>=64){
      throw std::runtime_error("More than 64 bits in integer");
    }
    if(option::termplates){
      if(End == Big){
        return boost::str(boost::format("%s.read_unsigned_big(%d)")%stream % width);
      } else { 
        return boost::str(boost::format("%s.read_unsigned_little(%d)")%stream % width);
    }else{
    //TODO: Support little endian?
      if(End == Big){
        return boost::str(boost::format("read_unsigned_bits(%s,%d)")%stream % width);
      } else { 
        return boost::str(boost::format("read_unsigned_bits_littleendian(%s,%d)")%stream % width);     }
    }
  }
public:
  void p_constint(int width, const std::string value,const std::string &fail){
    check_int(width,fail);
    out << "if( "<< int_expr("str_current",width) << "!= "<<value<<"){"
        <<fail<<"}";
  }

  void p_constantarray(const constarray &c, const std::string fail){
    switch(c.value.N_type){
    case STRING:
      assert(mk_str(c.parser.unsign) == "8");
      FOREACH(ch, c.value.string){
        p_constint(8,(boost::format("'%c'") % ch).str(),fail);
      }
      break;
    case VALUES:{
      int width = boost::lexical_cast<int>(mk_str(c.parser.unsign));
      FOREACH(v,c.value.values){
        p_constint(width, intconstant_value(*v),fail);
      }
      break;
    }
    }
  }
  void p_const(const constparser &c, const std::string fail){
    switch(c.N_type){
    case CARRAY:
      p_constantarray(c.carray,fail);
      break;
    case CREPEAT:{
      std::string retrylabel = (boost::format("constmany_%d_repeat") % num_iters++).str();
      std::string faillabel = (boost::format("constmany_%d_end") % num_iters++).str();
      out << retrylabel << ":\n";
      p_const(*c.crepeat,std::string("goto ")+faillabel+";");
      out << "goto " << retrylabel << ";\n";
      out << faillabel << ":\n";
      break;
    }
    case CINT:{
      int width = boost::lexical_cast<int>(mk_str(c.cint.parser.unsign));
      p_constint(width,intconstant_value(c.cint.value),fail);
    }
      break;
    case CREF:
      out <<"if(parser_fail(peg_" << mk_str(c.cref) << "(str_current))){\n"
          <<fail <<  "}\n";

      break;
    case CSTRUCT:
      FOREACH(field,c.cstruct){
        p_const(*field,fail);
      }
      break;
    case CUNION:{
      out << "{/*CUNION*/\n";
      if(option::templates){
          <<"strt_current::pos back = str_current.getpos();";
      } else {
        out << "stream_reposition(str_current, back);\n";
      }
      int thischoice = num_iters++;
      std::string succ_label = boost::str(boost::format("cunion_%d_succ") % thischoice);
      int n_option = 1;
      FOREACH(option,c.cunion){
        std::string fallthrough_label = boost::str(boost::format("cunion_%d_%d") % thischoice % n_option++);
        std::string failopt = boost::str(boost::format("goto %s;") % fallthrough_label);
        p_const(*option,failopt);
        out << "goto " << succ_label << ";\n";
        out << fallthrough_label << ":\n";
        if(option::templates)
          out << "str_current.rewind(back);\n";
        else
          out << "stream_reposition(str_current,back);\n";
      }
      out << fail << "\n";
      out << succ_label << ":;\n";
      out << "}\n";
    }
      break;
    }
  }
  void p_int(const constrainedint &c, Expr &lval, const std::string fail)
  {
    int width = boost::lexical_cast<int>(mk_str(c.parser.unsign));
    check_int(width,fail);
    out << "{\n " << lval << " = " << int_expr("str_current",width) << ";\n";
    if(c.constraint != NULL){
      out << "if(";
      std::stringstream val;
      val << lval;
      constraint(out,val.str(),*c.constraint);
      out << "){"
          <<fail
          <<"}\n";
    }
    out << "}\n";
  }
  void p_struct(const structparser &s, Expr &lval, const std::string fail, Scope &scope)
  {
    Scope newscope(scope);
    FOREACH(field, s){
      switch(field->N_type){
      case CONSTANT:
        p_const(field->constant, fail);
        break;
      case FIELD:{
        std::string name = mk_str(field->field.name);
        ValExpr fieldname(name,&lval);
        parser(field->field.parser->pr,fieldname,fail,newscope);
        break;
      }
      case DEPENDENCY:{        //TODO: Use BIND/TYPE/ETC
        int this_dep = ++nr_dep;
        std::string depfail = (boost::format("goto faildep_%d;") % this_dep).str();
        std::string type_suffix;
        std::string type = typedef_type(*field->dependency.parser,"", &type_suffix);
        std::string name(mk_str(field->dependency.name));
        assert(type_suffix == "");

        ValExpr lval(std::string ("dep_") + name);
        out << type << " dep_" << name << ";\n";
        parser(field->dependency.parser->pr,lval,fail,newscope);
        newscope.add_dependency_definition(name,type);
        break;
      }
    case TRANSFORM:{
      std::string streamname = mk_str(*stream);
      header << "extern int " << mk_str(field->transform.cfunction) << "_parse(NailArena *tmp";
      FOREACH(stream, field->transform.left){
        if(option::templates)
          out << ";\nstrt_" << streamname <<" str_"<<streamname<<";\n";
        else
          out << ";\nNailStream str_" << mk_str(*stream) <<";\n";
        // If declaration occurs right a label, we need an empty statement
      }
      out << "if(";
      out << mk_str(field->transform.cfunction) << "_parse(tmparena"; //TODO: use temp arena

      FOREACH(stream, field->transform.left){
        std::string streamstr = mk_str(*stream);
        if(option::templates){
          header << ", strt_"<< streamstr" *str_" << streamstr ;
          out << ", &str_"  << streamstr;
        } else {
          header << ",NailStream *str_" << streamstr;
          out << ", &str_"  << streamstr;
        }
      }      
      FOREACH(param, field->transform.right){
        switch(param->N_type){
        case PDEPENDENCY:{
          std::string name (mk_str(param->pdependency));
          out << "," << newscope.dependency_ptr(name); 
          header << "," << newscope.dependency_type(name) << "* " << name;
          break;
        }
        case PSTREAM:{
          std::string name = mk_str(param->pstream);
          out << "," <<  newscope.stream_ptr(name);
          if(option::templates)
            header << ",strt_"<<name<<" *" << name;
          else
            header << ",NailStream *" << name;
          break;
        }
        }
      }
      out << ")) {" << fail << "}";
      header << ");\n";
      FOREACH(stream, field->transform.left){
        newscope.add_stream_definition(mk_str(*stream));
      }
      break;
    }
      }
    }
  }

  
  void parser(const parserinner &p, Expr &lval, const std::string fail, Scope &scope){
    switch(p.N_type){
    case INTEGER:
      p_int(p.integer,lval, fail);
      break;
    case STRUCTURE:
      p_struct(p.structure,lval, fail,scope);
      break;
    case WRAP:
      if(p.wrap.constbefore){
        FOREACH(c,*p.wrap.constbefore){
          p_const(*c,fail);
        }
      }
      parser(p.wrap.parser->pr,lval,fail,scope);
      if(p.wrap.constafter){
        FOREACH(c,*p.wrap.constafter){
          p_const(*c,fail);
        }
      }
      break;
    case CHOICE:
      {
        int this_choice = num_iters++;
        if(option::templates){
          out << "{/*CHOICE*/strt_current::pos back = str_current.getpos();\n";
        } else {
          out << "{/*CHOICE*/NailStreamPos back = stream_getpos(str_current);\n";
        }
        out << "NailArenaPos back_arena = n_arena_save(arena);";
          std::string success_label = (boost::format("choice_%d_succ") % this_choice).str();
    
        FOREACH(c, p.choice){
          std::string tag (mk_str(c->tag));
          boost::algorithm::to_lower(tag);
          std::string fallthrough_memo  =  (boost::format("choice_%u_%s_out") % this_choice % tag).str();
          std::string fallthrough_goto = boost::str(boost::format("goto %s;") % fallthrough_memo);
          ValExpr expr(tag,&lval);
          parser(c->parser->pr,expr, fallthrough_goto,scope);
          out << ValExpr("N_type", &lval) << "= "<< mk_str(c->tag) <<";\n";
          out << "goto " << success_label<< ";\n";
          out << fallthrough_memo << ":\n";
          out << "n_arena_restore(arena, back_arena);\n";
          if(option::templates){
            out << "str_current.rewind(back);\n";
          }else {
            out << "stream_reposition(str_current, back);\n";
          }
        }
        out << fail << "\n";
        out << success_label <<": ;\n";
        out << "}";

        break;
      }
    case ARRAY:{
      const struct parser *i;
      const constparser *separator = NULL;
      bool min = false;
      int this_many = num_iters++;
      std::string gotofail=(boost::format("goto fail_repeat_%d;") % this_many).str();
      std::string gotofailsep=(boost::format("goto fail_repeat_sep_%d;") % this_many).str();
      ValExpr count("count", &lval);
      ValExpr data("elem", &lval);
      switch(p.array.N_type){
      case MANYONE:
        i = p.array.manyone;
        min = true;
        break;
      case MANY:
        i = p.array.many; break;
      case SEPBY:
        i = p.array.sepby.inner;
        separator = &p.array.sepby.separator; break;
      case SEPBYONE:
        i= p.array.sepbyone.inner;
        separator = &p.array.sepbyone.separator; 
        min =true;
        break;
      }
      out << "{\n";
      std::string temp = boost::str(boost::format("tmp_%d") % this_many);
      std::string iter = boost::str(boost::format("count_%d") % this_many);
      ValExpr iexpr(iter);
      ArrayElemExpr elem(&data,&iexpr);
      DerefExpr linkedlist = DerefExpr(ValExpr(temp));
      ValExpr tmpexpr("elem",&linkedlist);
      out << "pos " << iexpr << "= 0;\n";
      out << "struct { typeof("<<elem<<") elem; void *prev;} *"<< temp << " = NULL;\n";
      if(separator != NULL)
        out << "goto start_repeat_"<< this_many << ";\n";
      out  << "succ_repeat_" << this_many << ":;\n";
      if(option::template)
        out << "strt_current::pos posrep_" << this_many << "= str_current.getpos();";
      else
        out << "NailStreamPos posrep_" << this_many << "= stream_getpos(str_current);"; 
      out << "NailArenaPos back_arena = n_arena_save(arena);\n";
      if(separator != NULL){
        p_const(*separator,gotofailsep); // Skip backtracking temp when failing the separator
        out << "start_repeat_" << this_many <<":;\n";
      }
      out << "typeof("<<temp<<") prev_"<<temp<<"= "<<temp<<";\n"  
          << temp << " = n_malloc(tmparena,sizeof(*"<<temp<<"));\n"
          << "if(parser_fail(!"<<temp<<")) {return -1;}"
          << temp << "->prev = prev_"<<temp<<";\n";
      parser(i->pr,tmpexpr,gotofail, scope );
      out << iter << "++;\n";
      out << " goto succ_repeat_"<< this_many << ";\n";
      out << "fail_repeat_" << this_many << ":\n";
      out << temp << "= "<<temp<<"->prev;\n";
      out << "fail_repeat_sep_" << this_many << ":\n";
      out << "n_arena_restore(arena, back_arena);\n";
      if(option::templates)
        out << "str_current.rewind(posrep_"<<this_many<<");\n";
      else
        out << "stream_reposition(str_current, posrep_"<<this_many<<");\n";
      if(min){
        out << "if("<<iexpr<<"==0){"<< fail << "}\n";
      }
      
      out << count << "= "<<iexpr<<";\n";
      out << data << "= n_malloc(arena,sizeof("<<elem<<")*"<<count <<");\n"
          << "if(parser_fail(!"<<data<<")){ return -1;}";
      out << "while("<<iter<<"){"
          << iter<< "--;\n"
          << "memcpy(&"<<elem <<",&"<<temp<<"->elem,sizeof("<<elem<<"));"
          <<  temp << " = " << temp << "->prev;\n"
          << "}";
      out << "}";
      break;
    }
    case FIXEDARRAY:
      assert(!"unimplemented");
      break;
    case LENGTH:{
      std::string iter = boost::str(boost::format("i%d") % num_iters++);
      ValExpr count("count", &lval);
      ValExpr data("elem", &lval);
      ValExpr iexpr(iter);
      ArrayElemExpr elem(&data,&iexpr);
      out << "pos "<<iter<<" = 0 ;\n";
      out << count << "= dep_"<<mk_str(p.length.length) << ";\n";
      out << data << " = n_malloc(arena,"<<count<<"*sizeof("<<elem<<"));";
      out << "if(parser_fail(!"<<data<<")){return -1;}\n";
      out << "for(;"<<iter<<"<"<<count<<";"<<iter<<"++){";
      parser(p.length.parser->pr,elem,fail,scope);
      out << "}\n";
      break;
    }
    case APPLY:{
      int this_many = num_iters++;
      out << "{/*APPLY*/";
      if(option::templates){
        out << "strt_current * origstr_"<< this_many <<" = str_current;\n";
        out << "strt_current::pos *back_"<<this_many <<        "=str_current.getpos();";
      }
      else{
        out << "NailStream * origstr_"<< this_many <<" = str_current;\n";
        out << "NailStreamPos back_"<<this_many <<        "= stream_getpos(str_current);";
      }
      out << "str_current = " << scope.stream_ptr(mk_str(p.apply.stream)) << ";\n";
      parser (p.apply.inner->pr, lval,(boost::format("goto fail_apply_%d;") % this_many).str(), scope);
      out << "goto succ_apply_" << this_many << ";\n";
      out << "fail_apply_" << this_many << ":\n";
      out << "str_current = origstr_"<<this_many<<"; \n";
      if(option::templates)
        out << "str_current.rewind(back_"<<this_many<<");\n";
      else
        out << "stream_reposition(str_current, back_"<<this_many<<");\n"
      out << fail << "\n";
      out << "succ_apply_" << this_many << ":\n"
          << "str_current = origstr_"<<this_many<<";\n";      
      out << "}\n";
      break;
    }

      break;
    case OPTIONAL:{
      DerefExpr deref(lval);
      int this_many = num_iters++;
      out << "{/*Optional*/\n";
      if(option::templates).
        out << "strt_current back_"<< this_many<<"= str_current.getpos();";
      else
        out << "NailStreamPos back_"<< this_many<<"= stream_getpos(str_current);";
      out << "NailArenaPos back_"<<this_many<<" = n_arena_save(arena);";
        parser(p.optional->pr,deref, (boost::format("goto fail_optional_%d;") % this_many).str(), scope);
      out << " goto succ_optional_" << this_many <<  ";\n";
      out << "fail_optional_" << this_many << ":\n";
      out << "n_arena_restore(arena,back_"<<this_many<<");\n";
      out << lval << "= NULL;";
      if(option::templates).
        out << "str_current.rewind(back_"<<this_many<<");";
      else
        out << "stream_reposition(str_current,back_"<< this_many<<");\n";
      out << "succ_optional_" << this_many << ":\n";
      out << "}";
    }
      break;
    case REF:{
      std::string parameters =   parameter_invocation(p.ref.parameters,scope);
      out << lval << "= (typeof("<<lval<<")) n_malloc("<<arena<<",sizeof(*"<<lval<<"));\n";
      out << "if(!"<<lval<<"){return -1;}\n";
      out << "if(parser_fail(peg_"<<mk_str(p.name.name)<<"(arena,tmparena, "<<lval<<",str_current "<<parameters<<"))) { "<<fail<<"}\n";
      break;
    }
      
    case NAME:{
      std::string parameters =   parameter_invocation(p.ref.parameters,scope);
      out << "if(parser_fail(peg_"<<mk_str(p.name.name)<<"(arena,tmparena, &"<<lval<<",str_current "<<parameters<<"))) { "<<fail<<"}\n";
      break;
    }
    default:
      std::cerr << "BUG"<< std::endl;
      exit(-1);
    }
  }
  //XXX::We will need templates here!
  void emit_parser(grammar &gram){
    DONT_COMPILE_NEED_TEMPLATES_HERE
    std::stringstream forward_declarations;
    FOREACH(def,gram){
    if(def->N_type == ENDIAN){  
      End = def->endian.N_type == BIG ? Big : Little;
    } else if (def->N_type == PARSER){
      Scope scope;
      std::string name = mk_str(def->parser.name);
      scope.add_stream_parameter("current");
      std::string params = parameter_definition(*def,scope);
      out <<"static pos peg_" << name <<"(NailArena *arena,NailArena *tmparena,"<<mk_str(def->parser.name) << " *out,NailStream *str_current"<<params<<"){\n";
      out << "pos i;\n"; //Used in name and ref as temp variables
      ValExpr outexpr("out",NULL,1);
      parser( def->parser.definition.pr,outexpr, "goto fail;",scope);
      out << "return 0;\n"
          << "fail:\n return -1;\n";
      out << "}\n";
      forward_declarations << "static pos peg_" << mk_str(def->parser.name) <<"(NailArena *arena,NailArena *tmparena,"<<mk_str(def->parser.name) << " *out,NailStream *str_current"<<params<<");\n";
      if(!def->parser.parameters || def->parser.parameters->count==0){
        //XXX: Parameter pass thru
        //XXX: n_malloc
        out << name << "* parse_"<<name << "(NailArena *arena, NailStream *stream){\n"
            << name << "*retval = n_malloc(arena, sizeof(*retval));"
            << "NailArena tmparena;"
            <<"NailArena_init(&tmparena, 4096);"
            << "if(!retval) return NULL;\n"
            << "if(parser_fail(peg_"<<name<<"(arena, &tmparena,retval, stream))){"
            << "goto fail;"
            << "}"
            <<" if(!stream_check(stream,8)) {goto fail;}"
            << "NailArena_release(&tmparena);"
            << "return retval;\n"
            << " fail: " // Undo memory allocations on failed arena? 
            << "NailArena_release(&tmparena);"
            << "return NULL;"
            <<"}";
          }
    }  else if(def->N_type == CONSTANTDEF){
      std::string name = mk_str(def->constantdef.name);
      out << "static pos peg_" << name << "(NailStream *str_current){\n";
      p_const(def->constantdef.definition, "goto fail;");
      out << "return 0;\n"
          << "fail: return -1;\n"
          << "}";
      forward_declarations << "static pos peg_" << mk_str(def->constantdef.name) <<"(NailStream *str_current);\n";
    }

    }
    out << std::endl;
    final << header.str() << forward_declarations.str() << out.str() << std::endl;
    hdr << header.str() << std::endl;
  }
  CDirectParser(std::ostream *os, std::ostream *hedr): final(*os), hdr(*hedr), nr_dep(0), num_iters(1), End(Big){
  }
  
};
void emit_directparser(std::ostream *out, std::ostream *header, grammar *grammar){
  CDirectParser p(out,header);
  
  *out << std::string(parser_template_start,parser_template_end - parser_template_start);
  p.emit_parser(*grammar);
}
