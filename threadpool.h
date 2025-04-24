#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <unordered_map>
#include <iostream>

class Any
{
public:
    Any() = default;
    ~Any() = default;
    Any(const Any&) = delete;   // 禁止拷贝构造函数
    Any& operator=(const Any&) = delete;    // 禁止拷贝赋值函数
    Any(Any&&) = default;   // 允许移动构造函数
    Any& operator=(Any&&) = default;    // 允许移动赋值函数

    // 这个构造函数可以让Any类型接收任意其它的数据
    template<typename T>
    Any(T data) : base_(std::make_unique<Derive<T>>(data)) {}

    // 这个方法能把Any对象里面存储的data数据提取出来
    template<typename T>
    T cast_()
    {
        // 我们怎么从base_找到它所指向的Derive对象，从它里面取出data成员变量
        // 基类指针 =》 派生类指针   RTTI
        // 1. 先把base_转换成Derive<T>类型的指针
        // 2. 然后通过这个指针访问data成员变量
        Derive<T>* pd = dynamic_cast<Derive<T>*>(base_.get());
        if (pd == nullptr)
        {
            throw "type is unmatch";
        }
        return pd->data_;
    }

private:
    // 基类类型
    class Base
    {
    public:
        virtual ~Base() = default;
    };

    // 派生类类型
    template<typename T>
    class Derive : public Base
    {
    public:
        Derive(T data) : data_(data) {}
        T data_;    // 保存了任意的其它类型
    };

private:
    // 定义一个基类的指针
    std::unique_ptr<Base> base_;

};

// 实现一个信号量类
class Semaphore
{
public:
    Semaphore(int limit = 0) : resLimit_(limit)
    , isExit_(false) 
    {}

    ~Semaphore()
    {
        isExit_ = true;
    }

    

    // 获取一个信号量资源
    void wait()
    {
        if (isExit_) 
        {
            return;
        }
        std::unique_lock<std::mutex> lock(mtx_);
        // 等待信号量有资源，没有资源的话，会阻塞当前线程
        while (resLimit_ <= 0)
        {
            cond_.wait(lock);
        }
        --resLimit_;
        /*
        // 也可以这样写
        cond_.wait(lock, [&]()->bool {return resLimit_ > 0; });
        --resLimit_;
        */
    }

    // 增加一个信号量资源
    void post()
    {
        if (isExit_) 
        {
            return;
        }
        std::unique_lock<std::mutex> lock(mtx_);
        ++resLimit_;
        // linux下condition_variable的析构函数什么也没做
        // 导致这里状态已经失效，无故阻塞
        cond_.notify_all(); // 通知条件变量wait的地方，可以起来干活了
    }

private:
    std::atomic_bool isExit_;
    int resLimit_;   // 资源限制
    std::mutex mtx_;    // 互斥锁
    std::condition_variable cond_;   // 条件变量
};

// Task类型的前置声明
class Task;

// 实现接收提交到线程池的task任务执行完成后的返回值类型Result
class Result
{
public:
    Result(bool isValid = true);
    ~Result() = default;

    // 禁用拷贝构造函数和拷贝赋值运算符
    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;

    // 禁用移动构造函数和移动赋值运算符
    Result(Result&&) = delete;
    Result& operator=(Result&&) = delete;

    // 问题一：setVal方法，获取任务执行完的返回值
    void setVal(Any any);

    // 问题二：get方法，用户调用这个方法获取task的返回值
    Any get();

private:
    Any any_;   // 存储任务的返回值
    Semaphore sem_;   // 线程通信信号量
    std::atomic<bool> isValid_;   // 返回值是否有效
};

// 任务抽象基类
class Task
{
public:
    Task();
    virtual ~Task() = default;
    void exec();
    void setResult(std::shared_ptr<Result> res);

    // 用户可以自定义任意任务类型，从Task继承，重写run方法，实现自定义任务处理
    virtual Any run() = 0;

public:
    // Result* result_;
    std::shared_ptr<Result> result_;
};


// 线程池支持的模式
enum class PoolMode
{
    MODE_FIXED,  // 固定数量的线程
    MODE_CACHED, // 线程数量可动态增长
};

// 线程类型
class Thread
{
public:
    // 线程函数对象类型
    using ThreadFunc = std::function<void(int)>;

    // 线程构造
    Thread(ThreadFunc func);
    // 线程析构
    ~Thread();
    // 线程启动
    void start();

    // 获取线程id
    int getId() const;

private:
    ThreadFunc func_;   // 线程函数对象
    static int generateId_;   // 生成线程ID
    int threadId_;   // 保存线程ID
};

/*
example:
ThreadPool pool;
pool.start(4);

class MyTask : public Task
{
    public:
        void run() { // 线程代码... }
};

pool.submitTask(std::make_shared<MyTask>());
*/

// 线程池类型
class ThreadPool
{
public:
    // 线程池构造
    ThreadPool();

    // 线程池析构
    ~ThreadPool();

    // 设置线程池的工作模式
    void setMode(PoolMode mode);

    // 设置task任务队列上线阈值
    void setTaskQueMaxThreshold(int threshold);

    // 设置线程池cached模式下线程阈值
    void setThreadSizeThreshold(int threshold);

    // 给线程池提交任务
    std::shared_ptr<Result> submitTask(std::shared_ptr<Task> sp);

    // 启动线程池
    void start(int initThreadSize = std::thread::hardware_concurrency());

    ThreadPool(const ThreadPool&) = delete;   // 禁止拷贝构造函数
    ThreadPool& operator=(const ThreadPool&) = delete;   // 禁止拷贝赋值函数

private:
    // 定义线程函数
    void threadFunc(int threadid);

    // 检查pool的运行状态
    bool checkRunningState() const;

private:
    std::unordered_map<int, std::unique_ptr<Thread>> threads_;  // 线程列表

    size_t initThreadSize_;   // 线程池初始化线程数量
    int threadSizeThreshold_;   // 线程数量上限阈值
    std::atomic_int curThreadSize_; // 记录当前线程池里面线程的总数量
    std::atomic_int idleThreadSize_; // 记录当前空闲线程的数量

    std::queue<std::shared_ptr<Task>> taskQue_;   // 任务队列
    std::atomic_int taskSize_;   // 任务队列的任务数量
    int taskQueMaxThreshold_;   // 任务队列数量上限阈值

    std::mutex taskQueMtx_;   // 任务队列互斥锁，保证任务队列的线程安全
    std::condition_variable notFull_;   // 任务队列不满的条件变量
    std::condition_variable notEmpty_;   // 任务队列不空的条件变量
    std::condition_variable exitCond_;   // 等到线程资源全部回收

    PoolMode poolMode_;   // 当前线程池的工作模式
    std::atomic_bool isPoolRunning_;   // 表示当前线程池的启动状态
};

#endif
