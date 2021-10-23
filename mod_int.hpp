#pragma once

#include <limits>
#include <type_traits>
#include <cstddef>

template<typename T>
constexpr T max_multiple_inv(const T v)
{
   auto ret = (std::numeric_limits<T>::max() - v + 1)%v;
 
   return ret==0?v:ret;
}

template<typename T>
constexpr bool is_power_of_2(const T val) 
{
   if(val == 0) return false;
   else if(val == 1) return true;
   else if(val & 1) return false; 
   else return is_power_of_2(val >> 1);
}

template<typename T>
constexpr T max_multiple(const T v)
{
   if (is_power_of_2(v)) 
      return 0;
   else
   return std::numeric_limits<T>::max() - max_multiple_inv(v) + 1;
}

template<typename INT_TYPE ,INT_TYPE mod_val>
class mod_int
{
   public:

   using this_t = mod_int<INT_TYPE, mod_val>;

   mod_int(INT_TYPE val_in)
   {
      if(!mod_val)
         val_ = val_in;
      else
         val_ = val_in%mod_val;
   };

   mod_int operator + (this_t incr_val) const
   {
      if (!mod_val)
      {
         return this->val_ + incr_val.val_;
      }
      else
      {
         INT_TYPE new_val = (val_ + incr_val.val_);

         if (!is_power_of_2(mod_val)) 
         {
            if(new_val < val_){ new_val += max_multiple_inv(mod_val);}
         }

         return new_val%mod_val;
      }
   }

   mod_int operator ++ (int)
   {
      auto tmp = *this;
      operator++();
      return tmp;
   }

   mod_int operator -- (int)
   {
      auto tmp = *this;
      operator--();
      return tmp;
   }

   INT_TYPE val() const {return val_;}

   mod_int operator += (this_t incr_val){return (*this = *this + incr_val);}
   mod_int operator -= (this_t incr_val){ return operator+=(-incr_val);}

   mod_int operator ++ (){return operator+=(1); }
   mod_int operator -- (){return operator-=(1); }

   mod_int operator - (){return mod_val - this->val_;}
   mod_int operator - (this_t incr_val) const {return operator +(-incr_val);}

   bool operator == (const INT_TYPE val_in) const { return (val_ == val_in); }
   bool operator == (const mod_int val_in) const { return (val_ == val_in.val_);}
   bool operator != (const mod_int val_in) const { return !(val_ == val_in.val_);}
   
   bool operator > (const mod_int val_in) const { return (val_ > val_in.val_);}

   private:
   INT_TYPE val_;
};

template<typename T,size_t S>
class mod_index:public mod_int<T,max_multiple<T>(S)>
{
   public:

   using P = mod_int<T,max_multiple<T>(S)>;

   mod_index(T v):P(v){}
   mod_index(P v):P(v){}
   using P::val;

   T index()
   {
      return val()%S; 
   }
};