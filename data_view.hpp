#pragma once
// Type your code here, or load an example.
#include <array>
#include <cstddef>
#include <vector>

template<typename T>
class data_view
{
   public:
   data_view()
   {

   }
   data_view(T*a,size_t s)
   {

      begin_ = a; 
      end_ = a + s;

   }
   data_view(T*a,T*b)
   {
      begin_ = a; 
      end_ = b;
   }
    
   size_t size()
   {
      if(end_ && begin_)
         return end_ - begin_;
      else
         return 0;
   }
   T * begin(){return begin_;}
   T * end(){return end_;}
   private:
   T* begin_ = nullptr;
   T* end_ = nullptr;
};

template<typename T,size_t s>
class mdata_view 
{
    public:
   size_t size()
   {
      size_t sum = 0;
      for(auto i:data_)sum+=i.size();
      return sum;
   }
   size_t block_cnt()
   {
      return data_.size();
   } 
   data_view<T>& operator[](int i)
   {
      return data_[i];
   }
   std::vector<T> to_vec()
   {
      std::vector<T> ret((int)size());
      auto tmp = ret.begin();
      for(auto i :data_)
      {
         tmp = std::copy(i.begin(),i.end(),tmp);
      }
      return ret;
   }

   size_t copy_to(T * data)
   {
      mdata_view<T,1> tmp;
      tmp[0] = {data,size()};
      return copy_to(tmp);
   }

   template<size_t S2>
   size_t copy_to(mdata_view<T,S2> & dst)
   {
      size_t src_index= 0;
      size_t src_offset = 0;
      size_t dst_index = 0;
      size_t dst_offset = 0;

      while((src_index != block_cnt()) &&  (dst_index != dst.block_cnt()))
      {
         size_t size_src  = data_[src_index].size() - src_offset;
         size_t size_dst  = dst[dst_index].size() - dst_offset;

         if(size_src >= size_dst)
         {
            std::copy
            (
               data_[src_index].begin() + src_offset,
               data_[src_index].begin() + src_offset + size_dst ,
               dst[dst_index].begin() + dst_offset
            );
            ++dst_index; 
            dst_offset = 0;
            if(size_src == size_dst)
            {
               ++src_index; 
               src_offset = 0;
            }
            else
            {
               src_offset += size_dst;  
            }
         }
         else
         {
            std::copy
            (
               data_[src_index].begin() + src_offset,
               data_[src_index].end(),
               dst[dst_index].begin() + dst_offset
            );
            ++src_index; 
            src_offset = 0;
         }
      }

      size_t written = 0;
      #if 0
      for(auto i:data_)
      {
         auto res = std::copy(i.begin(),i.end(),dst+written);
         written += (res - i.begin());
      }
      #endif
      return written;
   }
   data_view<T> & operator[](size_t v)
   {
      return data_[v]; 
   }
   private:

   std::array<data_view<T>,s> data_;
};