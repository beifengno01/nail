#include "nailtool.h"
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>

#define p_arg(x) x.count,(const char*)x.elem

#define mk_str(x) std::string((const char *)x.elem,x.count)
#define printf(...)  ERROR

static unsigned long strntoud(unsigned siz,const char *num){
  unsigned i=0;
  unsigned long ret=0;
  for(;i<siz;i++){
    assert(num[i] >= '0' && num[i] <= '9');
    ret= 10 * ret+ num[i] - '0';
  }
  return ret;
}
class GenGenerator{
  std::ostream &out;
  int num_iters;
  void constint(int width, std::string value){
    out << "h_bit_writer_put(out," << value << "," << width << ");\n";
  }
  void generator(constarray &c){ 
    switch(c.value.N_type){
    case STRING:
      assert(mk_str(c.parser.UNSIGNED) == "8");
      FOREACH(ch, c.value.STRING){
        constint(8,(boost::format("'%c'") % ch).str());
      }
      break;
    case VALUES:{
      int width = boost::lexical_cast<int>(mk_str(c.parser.UNSIGNED));
      FOREACH(v,c.value.VALUES){
        constint(width, intconstant_value(*v));
      }
      break;
    }
    }
  }
  void generator(constparser &p){
    switch(p.N_type){
    case CARRAY:
      generator(p.CARRAY); break;
    case CREPEAT:
      generator(*p.CREPEAT); break; //Emit just one of the elements
    case CINT: {
      int width = boost::lexical_cast<int>(mk_str(p.CINT.parser.UNSIGNED));
      constint(width, intconstant_value(p.CINT.value));
    }
      break;
    case CREF:
      out << "gen_"<< mk_str(p.CREF)<<"(out);";break;
    case CSTRUCT:
      FOREACH(field,p.CSTRUCT){
        generator(*field);
      }
      break;
    case CUNION:
      generator(p.CUNION.elem[0]);
      break;
    }
  }
  void generator(parserinner &p,Expr &val){
    switch(p.N_type){
    case INT:{
      int width = boost::lexical_cast<int>(mk_str(p.INT.parser.UNSIGNED));
      out << "h_bit_writer_put(out,"<<val<<","<< width << ");";
      break;
    }
    case STRUCT:
      FOREACH(field, p.STRUCT){
        switch(field->N_type){
        case CONSTANT:
          generator(field->CONSTANT);
          break;
        case FIELD:
          ValExpr fieldname(mk_str(field->FIELD.name),&val);
          generator(field->FIELD.parser->PR,fieldname);
          break;
        }
      }
      break;
    case WRAP:
      if(p.WRAP.constbefore){
        FOREACH(c,*p.WRAP.constbefore){
          generator(*c);
        }
      }
      generator(p.WRAP.parser->PR,val);
      if(p.WRAP.constafter){
        FOREACH(c,*p.WRAP.constafter){
          generator(*c);
        }
      }      
      break;

    case CHOICE:
      {
        out << "switch("<< ValExpr("N_type",&val)<<"){\n";
        FOREACH(c, p.CHOICE){
          std::string tag = mk_str(c->tag);
          out << "case " << tag << ":\n";
          ValExpr expr(tag,&val);
          generator(c->parser->PR, expr );
          out << "break;\n";
        }
        out << "}";
      }      
      break;
    case UNION:
      {
        //What to do with UNION? Is union a bijection - 
        // Parentheses or the like might be NECESSARY for bijection. For now, fail?
        fprintf(stderr,"Warning, UNION is dangerous\n");
        generator(p.UNION.elem[0]->PR,val);
      }
      break;
    case ARRAY:
      {
        ValExpr count("count", &val);
        ValExpr data("elem", &val);
        
        ValExpr iter(boost::str(boost::format("i%d") % num_iters++));
        ArrayElemExpr elem(&data,&iter);
        out << "for(int "<< iter<<"=0;"<<iter << "<" << count << ";" << iter << "++){";
        switch(p.ARRAY.N_type){
        case MANYONE:
          generator(p.ARRAY.MANYONE->PR,elem);break;
        case MANY:
          generator(p.ARRAY.MANY->PR,elem);break;
        case SEPBY:
          out << "if("<<iter<<"!= 0){";
          generator(p.ARRAY.SEPBY.separator);
          out << "}";
          generator(p.ARRAY.SEPBY.inner->PR,elem);break;
        case SEPBYONE:
          out << "if("<<iter<<"!= 0){";
          generator(p.ARRAY.SEPBYONE.separator);
          out << "}";
          generator(p.ARRAY.SEPBYONE.inner->PR,elem);break;
        }
        out << "}";
      }
      break;
    case OPTIONAL:
      {
        out << "if(NULL!="<<val << "){";
        DerefExpr opt(val);
        generator(p.OPTIONAL->PR,opt);
        out << "}";
      }
      break;
    case REF:
      out << "gen_" << mk_str(p.REF) << "(out,"<< val << ");";
      break;
    case NAME:
      out << "gen_"<< mk_str(p.NAME) << "(out,&"<< val << ");";
      break;
    }
  }

public:
  GenGenerator(std::ostream &_out ) : out(_out), num_iters(0){ 
  }  
  void generator( grammar *grammar)
  {
    out << "#include <hammer/hammer.h>\n";
    FOREACH(definition,*grammar){

      if(definition->N_type == CONST){
        std::string name = mk_str(definition->CONST.name);
        out<<"void gen_"<<name<<"(HBitWriter* out);";
      }
      else if(definition->N_type==PARSER){
        std::string name = mk_str(definition->PARSER.name);
        out << "void gen_" << (name)<<"(HBitWriter *out,"<< name << " * val);";
      }          
    }
    FOREACH(definition,*grammar){

      if(definition->N_type == CONST){
        std::string name = mk_str(definition->CONST.name);
        out<<"void gen_"<<name<<"(HBitWriter* out){\n";
        generator(definition->CONST.definition);
        out << "}";
      }
      else if(definition->N_type==PARSER){
        std::string name = mk_str(definition->PARSER.name);
        out << "void gen_" << (name)<<"(HBitWriter *out,"<< name << " * val){";
        ValExpr outval("val",NULL,1);
        generator(definition->PARSER.definition.PR,outval);
        out << "}";
      }          
    }

  }
};
void emit_generator(std::ostream *out, grammar *grammar){
  GenGenerator g(*out);
  g.generator(grammar);
  *out << std::endl;
}
