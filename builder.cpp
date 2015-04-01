#include "builder.h"

#include <memory>

#include "parser.h"


namespace fp = franca::parser;
namespace fm = franca::model;


// FIXME implement duplicate checks


namespace /* anonymous */
{
   
// forward decl
fm::type* internal_resolve_unresolved(fm::type*, fm::typecollection&);
   
   
/**
 * placeholder during model generation if requested type is not yet
 * available, will hopefully be replaced on second pass through model.
 */
struct unresolved : fm::type
{
   unresolved(const std::string& name)
    : fm::type(name)
   {
      // NOOP
   }
};


void internal_resolve_unresolved(fm::enumeration& e)
{   
   e.base_ = internal_resolve_unresolved(e.base_, *e.parent_);
}


void internal_resolve_unresolved(fm::struct_& s)
{   
   std::for_each(s.members_.begin(), s.members_.end(), [&s](std::pair<fm::type*, std::string>& m){ m.first = internal_resolve_unresolved(m.first, *s.parent_); });   
      
   s.base_ = internal_resolve_unresolved(s.base_, *s.parent_);   
}


fm::type* internal_resolve_unresolved(fm::type* t, fm::typecollection& context)
{
   if (t)
   {
      unresolved* udef = dynamic_cast<unresolved*>(t);
      
      if (udef != 0)
      {   
         fm::type* rc = context.resolve(udef->name());
         
         if (rc)
         {
            delete t;         
            return rc;
         }
         else
            throw std::runtime_error("cannot resolve type");
      }
   }
 
   return t;
}


void internal_resolve_unresolved(fm::attribute& a)
{
   a.type_ = internal_resolve_unresolved(a.type_, a.interface_);   
}


void internal_resolve_unresolved(fm::method& m)
{
   std::for_each(m.in_.begin(), m.in_.end(), [&m](fm::arg& a){ a.type_ = internal_resolve_unresolved(a.type_, m.get_interface()); });
   std::for_each(m.out_.begin(), m.out_.end(), [&m](fm::arg& a){ a.type_ = internal_resolve_unresolved(a.type_, m.get_interface()); });
   
   m.errors_ = internal_resolve_unresolved(m.errors_, m.get_interface());
}


void internal_resolve_unresolved(fm::fire_and_forget_method& m)
{
   std::for_each(m.args_.begin(), m.args_.end(), [&m](fm::arg& a){ a.type_ = internal_resolve_unresolved(a.type_, m.interface_); });
}


void internal_resolve_unresolved(fm::broadcast& b)
{
   std::for_each(b.args_.begin(), b.args_.end(), [&b](fm::arg& a){ a.type_ = internal_resolve_unresolved(a.type_, b.interface_); });
}


void internal_resolve_unresolved_tc(fm::typecollection& coll)
{
   std::for_each(coll.types_.begin(), coll.types_.end(), [](fm::type* t){ 
             
      fm::struct_* s = dynamic_cast<fm::struct_*>(t);
            
      if (s)
      {      
         internal_resolve_unresolved(*s);             
      }
      else
      {               
         fm::enumeration* e = dynamic_cast<fm::enumeration*>(t);
         
         if (e)         
            internal_resolve_unresolved(*e);                   
      }       
      
   });
}


void internal_resolve_unresolved(fm::interface& iface)
{
   internal_resolve_unresolved_tc(iface);

   std::for_each(iface.attrs_.begin(), iface.attrs_.end(), [](fm::attribute& a){ internal_resolve_unresolved(a); });
   std::for_each(iface.methods_.begin(), iface.methods_.end(), [](fm::method& m){ internal_resolve_unresolved(m); });
   std::for_each(iface.ff_methods_.begin(), iface.ff_methods_.end(), [](fm::fire_and_forget_method& m){ internal_resolve_unresolved(m); });
   std::for_each(iface.broadcasts_.begin(), iface.broadcasts_.end(), [](fm::broadcast& b){ internal_resolve_unresolved(b); });         
}


void internal_resolve_unresolved(fm::package& pack)
{
   std::for_each(pack.collections_.begin(), pack.collections_.end(), [](fm::typecollection& coll){ internal_resolve_unresolved_tc(coll); });
   std::for_each(pack.interfaces_.begin(), pack.interfaces_.end(), [](fm::interface& iface){ internal_resolve_unresolved(iface); });
   
   std::for_each(pack.packages_.begin(), pack.packages_.end(), [](fm::package& pck){ internal_resolve_unresolved(pck); });
}

}   // namespace


// ---------------------------------------------------------------------


struct method_error_builder : public boost::static_visitor<fm::enumeration*>
{
   method_error_builder(fm::method& m)
    : method_(m)
   {
      // NOOP
   }
   
   fm::enumeration* operator()(const std::string& err) const
   {
      fm::type* t = method_.get_interface().resolve(err);
      
      if (t)
         return dynamic_cast<fm::enumeration*>(t);      
      
      throw std::runtime_error("unknown error enum type");      
   }
   
   fm::enumeration* operator()(const fp::extended_error& err) const
   {
      std::unique_ptr<fm::enumeration> e(new fm::enumeration(method_.name() + "_errors", method_.get_interface()));
      
      if (err.base_)
      {
         fm::type* t = method_.get_interface().resolve(*err.base_);
         
         if (t)
         {
            if (dynamic_cast<fm::enumeration*>(t) == 0)
               throw std::runtime_error("invalid base error type");                                 
         }
         else
            t = new unresolved(*err.base_);
         
         e->base_ = t;
      }
      
      // iterate over all enum values
      for (auto iter = err.values_.begin(); iter != err.values_.end(); ++iter)
      {
         // FIXME check value - maybe use set instead here!
         // if can be resolved add the element to the struct
         e->enumerators_.push_back(fm::enumerator(iter->name_, iter->value_));         
      }
      
      return e.release();
   }
      
   fm::method& method_;
};


// ---------------------------------------------------------------------


struct typecollection_builder : public boost::static_visitor<void>
{
   typecollection_builder(fm::typecollection& coll)
    : coll_(coll)
   {
      // NOOP
   }
   
   void operator()(const fp::struct_& s) const
   {                   
      std::unique_ptr<fm::struct_> new_s(new fm::struct_(s.name_, coll_));
      
      // search for parent if apropriate
      if (s.base_)
      {         
         fm::type* base = coll_.resolve(*s.base_);
                  
         if (base)
         {
            if (dynamic_cast<fm::struct_*>(base) == 0)
               throw std::runtime_error("invalid struct base type");
         }
         else         
            base = new unresolved(*s.base_);         
                     
         new_s->base_ = base;         
      }
      
      // iterate over all elements of struct. 
      for (auto iter = s.values_.begin(); iter != s.values_.end(); ++iter)
      {         
         // resolve element
         fm::type* t = coll_.resolve(iter->type_);
         if (!t)             
            t = new unresolved(iter->type_);            
                     
         // if can be resolved add the element to the struct
         new_s->members_.push_back(std::make_pair(t, iter->name_));
      }
   
      // keep it
      new_s.release();
   }
   
   
   void operator()(const fp::enumeration& e) const
   {
      std::unique_ptr<fm::enumeration> new_e(new fm::enumeration(e.name_, coll_));
      
      // search for parent if apropriate
      if (e.base_)
      {         
         fm::type* base = coll_.resolve(*e.base_);
         
         if (base)
         {
            if (dynamic_cast<fm::enumeration*>(base) == 0)
               throw std::runtime_error("invalid enum base type");
         }
         else
            base = new unresolved(*e.base_);            
                     
         new_e->base_ = base;         
      }
      
      // iterate over all enum values
      for (auto iter = e.values_.begin(); iter != e.values_.end(); ++iter)
      {
         // FIXME check value - maybe use set instead here!
         // if can be resolved add the element to the struct
         new_e->enumerators_.push_back(fm::enumerator(iter->name_, iter->value_));         
      }
   
      // keep it
      new_e.release();
   }
   
   fm::typecollection& coll_;
};


// ---------------------------------------------------------------------


struct interface_builder : typecollection_builder
{
   using typecollection_builder::operator();
   
   
   interface_builder(fm::interface& i)
    : typecollection_builder(i)
    , i_(i)
   {
      // NOOP
   }   
   
   
   void operator()(const fp::method& s) const
   {
      fm::method meth(s.name_, i_);
          
      // in
      for(auto iter = s.in_.begin(); iter != s.in_.end(); ++iter)
      {
         fm::type* t = i_.resolve(iter->type_);
         if (!t)
            t = new unresolved(iter->type_);            
            
         meth.in_.push_back(fm::arg(iter->name_, t));
      }
      
      // out      
      for(auto iter = s.out_.begin(); iter != s.out_.end(); ++iter)
      {
         fm::type* t = i_.resolve(iter->type_);
         if (!t)
            t = new unresolved(iter->type_);    
            
         meth.out_.push_back(fm::arg(iter->name_, t));
      }
      
      // errors
      if (s.error_)      
      {
         meth.errors_ = boost::apply_visitor(method_error_builder(meth), *s.error_);
         
         if (!meth.errors_)
            return;
      }
      
      i_.methods_.push_back(meth);
   }
   
   
   void operator()(const fp::fire_and_forget_method& s) const
   {
      fm::fire_and_forget_method n_meth(s.name_, i_);
            
      for(auto iter = s.in_.begin(); iter != s.in_.end(); ++iter)
      {
         fm::type* t = i_.resolve(iter->type_);
         if (!t)
            t = new unresolved(iter->type_);
            
         n_meth.args_.push_back(fm::arg(iter->name_, t));
      }
      
      i_.ff_methods_.push_back(n_meth);
   }
   
   
   void operator()(const fp::broadcast& s) const
   {
      fm::broadcast bc(s.name_, i_);
            
      for(auto iter = s.args_.begin(); iter != s.args_.end(); ++iter)
      {
         fm::type* t = i_.resolve(iter->type_);
         if (!t)
            t = new unresolved(iter->type_);
            
         bc.args_.push_back(fm::arg(iter->name_, t));
      }
      
      i_.broadcasts_.push_back(bc);
   }
   
   
   void operator()(const fp::attribute& s) const
   {      
      fm::type* t = i_.resolve(s.type_);
      
      if (!t)
         t = new unresolved(s.type_);
      
      fm::attribute attr(s.name_, *t, i_, s.readonly_, s.no_subscriptions_);      
      
      i_.attrs_.push_back(attr);
   }
      
      
   fm::interface& i_;
};


// ---------------------------------------------------------------------


struct package_builder : public boost::static_visitor<void>
{
   package_builder(fm::package& package)
    : package_(package)
   {
      // NOOP
   }
   
   void operator()(const fp::interface& i) const
   {
      fm::interface mi(i.name_, i.version_.major_, i.version_.minor_);      
      fm::interface& rmi = package_.add_interface(mi);
      
      std::for_each(i.parseitems_.begin(), i.parseitems_.end(), [this,&rmi]( const fp::interface_item_type& item) {
         boost::apply_visitor(interface_builder(rmi), item);
      });
   }
   
   void operator()(const fp::typecollection& tc) const
   {
      fm::typecollection mtc(tc.name_);      
      fm::typecollection& rmtc = package_.add_typecollection(mtc);
      
      std::for_each(tc.parseitems_.begin(), tc.parseitems_.end(), [this,&rmtc]( const fp::tc_item_type& item) {
         boost::apply_visitor(typecollection_builder(rmtc), item);
      });
   }
   
   fm::package& package_;
};


// ---------------------------------------------------------------------


/*static*/ 
fm::package& franca::builder::build(fm::package& root, const fp::document& parsetree)
{
   fm::package* parent = &root;
      
   std::for_each(parsetree.package_.begin(), parsetree.package_.end(), [&parent](const std::string& str) {
      fm::package pck(str);
      parent = &parent->add_package(pck);      
   });
   
   std::for_each(parsetree.parseitems_.begin(), parsetree.parseitems_.end(), [parent]( const fp::doc_item_type& item) {
      boost::apply_visitor(package_builder(*parent), item);
   });
      
   return root;
}


/*static*/
void franca::builder::resolve_all_symbols(model::package& root)
{
   internal_resolve_unresolved(root);
}


/*static*/
void franca::builder::parse_and_build(model::package& root, const char* franca_file)
{
   fp::document doc = fp::parse(franca_file);
      
   (void)franca::builder::build(root, doc);
   franca::builder::resolve_all_symbols(root);   
}
