#include <iostream>
#include <chrono>
#include <string>
#include <ydb-global.h>
#include <unistd.h>
#include <thread>

#include <unistd.h>

int main(int argc, char** argv)
{

  if(argc != 4) {
    std::cout << "ERROR: Wrong amount of args" << std::endl;
    return -1;
  }

  std::string article_id = std::string(argv[1]);
  int number_of_set_calls = stoi(argv[2]);
  int ns_between_each_call = stoi(argv[3]);

  using namespace std::chrono; 

  c_ydb_global _articles("^articles");

  steady_clock::time_point start_point;
  duration<int64_t, std::nano> start_duration;

  for (int i=0; i<number_of_set_calls; i++) { 
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns_between_each_call));
    start_point = steady_clock::now();
    start_duration = duration_cast<duration<int64_t,std::nano>>(start_point.time_since_epoch());
    
    _articles[article_id]["title"] = std::to_string(start_duration.count());
    
    //std::this_thread::sleep_for(std::chrono::nanoseconds(7000000));
  }

  return 0;
}
