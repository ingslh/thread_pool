/**
 * 几个需要注意的点:
 * 1、tasks的读写锁需要优化成带优先级的锁, 可以肯定线程池的绝大部分使用场景commit task比run task更密集
 * 2、根据tasks以及cpu扩展线程数
 * 3、支持允许缓存的task数,如果超出此数将采取拒绝策略
 * 4、拒绝策略
 * 使用方式：
 * auto thread_pool = std::make_shared<ThreadPool>(items.size());
 * thread_pool->start();
 * for(auto item : items){
 *     thread_pool->commit(std::bing(&someProgressFunc, &object, std::cref(arg1), arg2));
 * }
 * thread_pool.reset();
*/
#pragma once
#include <memory>
#include <functional>
#include <future>
#include <condition_variable>
#include <queue>
#include <algorithm>
#include <vector>
#include <thread>
#include <typeinfo>
class ThreadPool {
public:

    ThreadPool(int core, int max = 0, int cache = 0): core_(core),
        max_(max), cache_(cache), quit_(false), force_(false) {

    }

    ~ThreadPool() {
        this->quit_.store(true);
        this->enable_.notify_all();
        std::for_each(this->pool_.begin(), this->pool_.end(), [](std::thread & t) {
            if (t.joinable()) {
                t.join();
            }
            });
    }
public:
    void start() {
        for (auto idx = 0; idx < core_; ++idx) {
            pool_.push_back(std::thread([this]() {
                // 第一次退出,判断是否要强制退出
                bool quit_ = this->force_.load() ? this->quit_.load() : false;
                for (; !quit_;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->oper_lock_);
                        this->enable_.wait(lock, [this]() {
                            return this->quit_.load() || !this->tasks_.empty();
                            });
                        // 不是强制退出时可从这里退出
                        if (this->quit_.load() && this->tasks_.empty()) {
                            return;
                        }
                        task = std::move(this->tasks_.front());
                        this->tasks_.pop();
                    }                    
                    task();
                }
                }));
        }
    }

    void shutdown(bool force = false) {
        this->quit_.store(true);
        this->force_.store(force);
    }

    //c++11:使用auto完成返回类型后置，使用decltype完成尾返回类型（trailing return type）
    //c++14中可省略尾返回类型，可定义为auto commit(T && t, Args&&...args)，auto实现自动推导
    template<class T, class... Args>
    auto commit(T && t, Args&&...args)->std::future<decltype(t(args...))> {//形参类型都为右值引用
        using TYPE = decltype(t(args...));
        if (this->quit_.load()) {
            //dont know return what, so throw an exception
            throw std::runtime_error("thread pool is alreay shutdown.");
        }
        auto task = std::make_shared<std::packaged_task<TYPE()> >(
            std::bind(std::forward<T>(t), std::forward<Args>(args)...)//使用std::forward进行参数转发（传递）,以保持原有的参数引用类型（左值引用，右值引用）
            );
        std::future<TYPE> result = task->get_future();
        std::lock_guard<std::mutex> lock(this->oper_lock_);
        this->tasks_.emplace([task]() { (*task)(); });
        this->enable_.notify_one();
        return result;
    }
private:
    std::vector<std::thread> pool_;
    std::queue<std::function<void()> > tasks_;
    int core_;   //线程池核心线程数
    int max_;    //线程池根据tasks量以及cpu数最大可扩展的量
    int cache_;  //运行tasks可缓存的最大task数,超出次数后commit将采取拒绝策略

    std::atomic<bool> quit_;     //线程池shutdown条件, true时shutdown
    std::atomic<bool> force_;    //是否强制shutdown,true时有剩余的task将不执行直接退出, false时等待执行完所有的task再退出
    std::condition_variable enable_;     //条件变量
    std::mutex oper_lock_;   // queue的读写锁
};
