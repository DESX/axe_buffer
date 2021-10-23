#include "mod_int.hpp"
#include "axe_buffer.hpp"
#include "data_view.hpp"
#include "fmt/format.h"
#include <catch2/catch_test_macros.hpp>

#include <experimental/array>
#include <vector>

TEST_CASE("aix buffer test","")
{
   SECTION("mdata_view test")
   {
      mdata_view<uint8_t,2> f;
      std::vector<uint8_t> tst_set = {0,1,2,3,4,5,6,7,8,9};

   }
   SECTION("is power of 2 test")
   {   

      REQUIRE(is_power_of_2<uint8_t>(1 ) == true); 
      REQUIRE(is_power_of_2<uint8_t>(2 ) == true); 
      REQUIRE(is_power_of_2<uint8_t>(4 ) == true); 
      REQUIRE(is_power_of_2<uint8_t>(8 ) == true); 
      REQUIRE(is_power_of_2<uint8_t>(16) == true); 
      REQUIRE(is_power_of_2<uint8_t>(32) == true); 
      REQUIRE(is_power_of_2<uint8_t>(64) == true); 
      REQUIRE(is_power_of_2<uint8_t>(128) == true); 

      REQUIRE(is_power_of_2<uint8_t>(  0) == false); 
      REQUIRE(is_power_of_2<uint8_t>(  3) == false); 
      REQUIRE(is_power_of_2<uint8_t>(  5) == false); 
      REQUIRE(is_power_of_2<uint8_t>(  6) == false); 
      REQUIRE(is_power_of_2<uint8_t>(  7) == false); 
      REQUIRE(is_power_of_2<uint8_t>(  9) == false); 
      REQUIRE(is_power_of_2<uint8_t>( 10) == false); 
      REQUIRE(is_power_of_2<uint8_t>( 11) == false); 
      REQUIRE(is_power_of_2<uint8_t>( 12) == false); 
      REQUIRE(is_power_of_2<uint8_t>( 13) == false); 
      REQUIRE(is_power_of_2<uint8_t>( 14) == false); 
      REQUIRE(is_power_of_2<uint8_t>( 33) == false); 
      REQUIRE(is_power_of_2<uint8_t>(255) == false); 

   }
   SECTION("max multiple test")
   {   
      //REQUIRE(max_multiple<uint8_t>(  0) ==   0);//floating point error signal
      REQUIRE(max_multiple<uint8_t>(  1) ==   0);
      REQUIRE(max_multiple<uint8_t>(  2) ==   0);
      REQUIRE(max_multiple<uint8_t>(  3) == 255);
      REQUIRE(max_multiple<uint8_t>(  4) ==   0);
      REQUIRE(max_multiple<uint8_t>(  5) == 255);
      REQUIRE(max_multiple<uint8_t>(  6) == 252);
      REQUIRE(max_multiple<uint8_t>(  7) == 252);
      REQUIRE(max_multiple<uint8_t>(  8) ==   0);
      REQUIRE(max_multiple<uint8_t>(  9) == 252);
      REQUIRE(max_multiple<uint8_t>( 10) == 250);
      REQUIRE(max_multiple<uint8_t>(100) == 200);
      REQUIRE(max_multiple<uint8_t>(200) == 200);
      REQUIRE(max_multiple<uint8_t>(250) == 250);
      REQUIRE(max_multiple<uint8_t>(255) == 255);
   }
   SECTION("test mod int")
   {   
      mod_int<uint8_t, 10> v = 4;
      auto b = ++v;
      REQUIRE(b == 5); 
      auto c = v++;
      REQUIRE(c == 5); 

      REQUIRE(v == 6); 
      v+=1;
      REQUIRE(v == 7); 
      v+=2;
      REQUIRE(v == 9); 
      v+=1;
      REQUIRE(v == 0); 
      v+=10;
      REQUIRE(v == 0); 
      v=9;
      REQUIRE(v == 9); 
      v+=250;
      REQUIRE(v == 9); 
      v+=255;
      REQUIRE(v == 4); 
      auto y =v + 1;
      REQUIRE(v == 4); 
      REQUIRE(y == 5); 
      v-=1;
      REQUIRE(v.val() == 3); 
      v-=3;
      REQUIRE(v.val() == 0); 
      v-=2;
      REQUIRE(v.val() == 8); 
      v-=10;
      REQUIRE(v.val() == 8); 
      v-=20;
      REQUIRE(v.val() == 8); 
      v-=250;
      REQUIRE(v.val() == 8); 
      mod_int<uint8_t, 0> q = 4;
      REQUIRE(q.val() == 4); 
      q +=251;
      REQUIRE(q.val() == 255);
      q +=1;
      REQUIRE(q.val() == 0); 
      q =1; 
      q += 255;
      REQUIRE(q.val() == 0); 

      mod_int<uint8_t, 10> r = 4;
      r += 255;
      REQUIRE(r.val() == 9);
      REQUIRE(r.val() == 9); 
      r = 9;
      r += 255;
      REQUIRE(r.val() == 4);
      {  
         mod_int<uint8_t, 10> v = 0;
         v-=2;
         REQUIRE(v.val() == 8);
      }
      mod_int<uint32_t, 10> ss = 1;

      REQUIRE(ss.val() == 1);
      ss = -ss;
      REQUIRE(ss.val() == 9);
      ss = 10;
      REQUIRE(ss.val() == 0);

   }
   SECTION("mod index test")
   {
      mod_index<uint8_t,5>  f1 = (uint8_t)22;
      REQUIRE(f1.val() == 22);
      REQUIRE(f1.index() == 2);
      f1++; 
      REQUIRE(f1.val() == 23);
      REQUIRE(f1.index() == 3);
      ++f1; 
      REQUIRE(f1.val() == 24);
      REQUIRE(f1.index() == 4);
      ++f1; 
      REQUIRE(f1.val() == 25);
      REQUIRE(f1.index() == 0);
      f1+=10; 
      REQUIRE(f1.val() == 35);
      REQUIRE(f1.index() == 0);
      f1+=100; 
      REQUIRE(f1.val() == 135);
      REQUIRE(f1.index() == 0);
      f1+=1; 
      REQUIRE(f1.val() == 136);
      REQUIRE(f1.index() == 1);
      f1+=110; 
      REQUIRE(f1.val() == 246);
      REQUIRE(f1.index() == 1);
      f1+=10; 
      REQUIRE(f1.val() ==   1);
      REQUIRE(f1.index() == 1);
      f1-=10; 
      REQUIRE(f1.val() == 246);
      REQUIRE(f1.index() == 1);
   }
   SECTION("EXE BLOCK TESTS")
   {
      using tp =  std::array<int,5>;
      using vec =  std::vector<int>;

      axe_block<int, 5> f1;  

      f1.buffer.fill(0);

      REQUIRE(f1.buffer == tp{0,0,0,0,0});

      auto i = f1.first_index();

      REQUIRE(i == 0); 
      f1[i] = 9;
      REQUIRE(f1.buffer == tp{ 9, 0, 0, 0, 0});
      f1[++i] = 10;
      REQUIRE(f1.buffer == tp{ 9,10, 0, 0, 0});
      f1[i+=11] = 11;
      REQUIRE(f1.buffer == tp{ 9,10,11, 0, 0});
      REQUIRE(i == 12); 
      f1[i+11] = 12;
      REQUIRE(f1.buffer == tp{ 9,10,11,12, 0});
      REQUIRE(i == 12); 
      REQUIRE(f1.get(0,2).to_vec() == vec{9,10});
      REQUIRE(f1.get(2,4).to_vec() == vec{11,12});

      REQUIRE(f1.get(i,i+2).to_vec() == vec{11,12});
      int out[2];
      f1.get(0,2).copy_to(out);
      
      REQUIRE(std::to_array(out) == std::to_array<int>({9,10}));
      
   }
   SECTION("data view tests")
   {
      mdata_view<char,2> d1;

      char a[5] = {'a','b','c','d','e'};
      char b[3] = {'f','g','h'};

      d1[0] = {a, sizeof(a)};
      d1[1] = {b, sizeof(b)};

      REQUIRE(d1.to_vec() == std::vector<char>{'a','b','c','d','e','f','g','h'});

      mdata_view<char,3> d2;

      char c[2];
      char d[3];
      char e[3];

      d2[0] = {c,sizeof(c)};
      d2[1] = {d,sizeof(d)};
      d2[2] = {e,sizeof(e)};

      d1.copy_to(d2);

      REQUIRE(d2.to_vec() == std::vector<char>{'a','b','c','d','e','f','g','h'});
   }
}
