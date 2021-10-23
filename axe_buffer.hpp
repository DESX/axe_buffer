#pragma once

#include <array>
#include <cstddef>

#include "mod_int.hpp"
#include "data_view.hpp"

template<typename T, size_t S>
class axe_block
{
   public:

   size_t size(){return S;}

   using idx = mod_index<T,S>;

   using dta2x = mdata_view<T,2>;

   constexpr static idx first_index() {return 0;}

#if 0
   template<typename T, size_t S>
   class axe_loc
   {
      public:
      axe_loc(axe_block& parent):p(parent){}
      private:
      parent & p;
      
   };
#endif
   using data2x = mdata_view<T,2>;
   data2x get_dataview(idx begin ,idx end)
   {
      data2x ret;
      if(begin.index() <= end.index())
      {
         ret[0] = {buffer.data() + begin.index(),buffer.data() + end.index()};
      }
      else
      {
         ret[0] = {buffer.data() + begin.index(),buffer.data() + size()};
         ret[1] = {buffer.data()                ,buffer.data() + end.index()};
      }
      return ret;
   }
   void insert(T * itm, size_t cnt, idx pos)
   {
      buffer[pos.index()] = itm;
   }
   T & operator[](idx pos)
   {
      return buffer[pos.index()];
   }
   T & get(idx pos)
   {
      return buffer[pos.index()];
   }
   dta2x  get(idx a,idx b)
   {
      dta2x ret;

      if ((b - a) > size())
      {
         a = b - size();
      }

      if(a == b) 
      {
         return ret; 
      }
      else if(a.index() < b.index())
      {
         ret[0] = {&buffer[a.index()],&buffer[b.index()]}; 
      }
      else
      {

         ret[0] = {&buffer[a.index()],&buffer[size()]}; 
         ret[1] = {&buffer[        0],&buffer[b.index()]}; 

      }
      return ret;
   }

   std::array<T, S> buffer;

};

template<typename T, size_t S>
class axe_buffer:public axe_block<T, S>
{

};