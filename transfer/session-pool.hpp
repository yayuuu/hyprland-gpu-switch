#pragma once
#include <list>
#include <memory>
#include <algorithm>
#include <cstddef>
namespace Offload {
template<class T> class SessionPool {
    std::list<std::unique_ptr<T>> entries;
    std::size_t maximum;
  public:
    explicit SessionPool(std::size_t limit):maximum(limit){}
    auto begin(){return entries.begin();} auto end(){return entries.end();}
    auto begin()const{return entries.begin();} auto end()const{return entries.end();}
    std::size_t size()const{return entries.size();}
    template<class Predicate,class Factory> T* acquire(Predicate matches,Factory create){
        for(auto& item:entries)if(matches(*item))return item.get();
        if(entries.size()>=maximum)return nullptr;
        auto entry=create();auto* result=entry.get();entries.push_back(std::move(entry));return result;
    }
    template<class Predicate> void eraseIf(Predicate retire){std::erase_if(entries,[&](const auto& item){return retire(*item);});}
    void clear(){entries.clear();}
};
}
