#include "threadpool.h"

#include <iostream>
#include <chrono>

const int TASK_MAX_THRESHOLD = INT32_MAX; // 任务数量最大阈值
const int THREAD_MAX_THRESHOLD = 1024;    // 线程数量最大阈值
const int THREAD_MAX_IDLE_TIME = 60;       // 单位：秒

// 线程池构造
ThreadPool::ThreadPool() : initThreadSize_(0), taskSize_(0), idleThreadSize_(0),
curThreadSize_(0), taskQueMaxThreshold_(TASK_MAX_THRESHOLD),
threadSizeThreshold_(THREAD_MAX_THRESHOLD),
poolMode_(PoolMode::MODE_FIXED), isPoolRunning_(false)
{
}

// 线程池析构
ThreadPool::~ThreadPool()
{
    isPoolRunning_ = false; // 设置线程池不运行，停止线程池

    // 等待线程池里面所有的线程返回  有三种状态：等待 & 阻塞 & 正在执行任务中
    std::unique_lock<std::mutex> lock(taskQueMtx_);
    notEmpty_.notify_all(); // 通知所有等待的线程
    exitCond_.wait(lock, [this]
        { return curThreadSize_ == 0; }); // 等待所有线程退出
    //exitCond_.wait(lock, [&]() -> bool
    //    { return threads_.size() == 0; });
}

// 设置线程池的工作模式
void ThreadPool::setMode(PoolMode mode)
{
    if (checkRunningState())
    {
        std::cerr << "ThreadPool is running, cannot set mode!" << std::endl;
        return;
    }
    poolMode_ = mode;
}

// 设置task任务队列上线阈值
void ThreadPool::setTaskQueMaxThreshold(int threshold)
{
    if (checkRunningState())
    {
        std::cerr << "ThreadPool is running, cannot set task queue max threshold!" << std::endl;
        return;
    }
    if (threshold <= 0 || threshold > TASK_MAX_THRESHOLD)
    {
        std::cerr << "Invalid task queue max threshold!" << std::endl;
        return;
    }
    taskQueMaxThreshold_ = threshold;
}

// 设置线程池cached模式下线程阈值
void ThreadPool::setThreadSizeThreshold(int threshold)
{
    if (checkRunningState())
    {
        std::cerr << "ThreadPool is running, cannot set thread size threshold!" << std::endl;
        return;
    }
    if (poolMode_ != PoolMode::MODE_CACHED)
    {
        std::cerr << "Thread size threshold can only be set in cached mode!" << std::endl;
        return;
    }
    if (threshold <= 0 || threshold > THREAD_MAX_THRESHOLD)
    {
        std::cerr << "Invalid thread size threshold!" << std::endl;
        return;
    }
    threadSizeThreshold_ = threshold;
}

// 给线程池提交任务    用户调用该接口，传入任务对象，生产任务
std::shared_ptr<Result> ThreadPool::submitTask(std::shared_ptr<Task> sp)
{
    // 获取锁
    std::unique_lock<std::mutex> lock(taskQueMtx_);

    // 线程的通信  等待任务队列有空余   wait   wait_for   wait_until
    // 用户提交任务，最长不能阻塞超过1s，否则判断提交任务失败，返回

    if (!notFull_.wait_for(lock, std::chrono::seconds(1),
        [&]() -> bool
        { return taskQue_.size() < (size_t)taskQueMaxThreshold_; }))
    {
        // 表示notFull_等待1s种，条件依然没有满足
        std::cerr << "Task queue is full, submit task failed!" << std::endl;
        return std::make_shared<Result>(false);
    }

    // 如果有空余，把任务放入任务队列中
    taskQue_.emplace(sp);
    ++taskSize_;

    // 因为新放了任务，任务队列肯定不空了，在notEmpty_上进行通知，赶快分配线程执行任务
    notEmpty_.notify_all();

    // cached模式 任务处理比较紧急 场景：小而快的任务 需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程出来
    if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshold_)
    {
        std::cout << ">>> create new thread..." << std::endl;

        // 创建新的线程对象
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
        // 启动线程
        threads_[threadId]->start();
        // 修改线程个数相关的变量
        curThreadSize_++;
        idleThreadSize_++;
    }

    auto res = std::make_shared<Result>();
    sp->setResult(res);
    return res;
}

// 开启线程池
void ThreadPool::start(int initThreadSize)
{
    // 设置线程池的运行状态
    isPoolRunning_ = true;

    // 记录初始线程个数
    initThreadSize_ = initThreadSize;
    curThreadSize_ = initThreadSize;

    // 创建线程对象
    for (int i = 0; i < initThreadSize_; i++)
    {
        // 创建thread线程对象的时候，把线程函数给到thread线程对象
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
    }

    // 启动所有线程
    for (int i = 0; i < initThreadSize_; i++)
    {
        threads_[i]->start(); // 需要去执行一个线程函数
        ++idleThreadSize_;    // 记录初始空闲线程的数量
    }
}

// 定义线程函数   线程池的所有线程从任务队列里面消费任务
void ThreadPool::threadFunc(int threadid)
{
    auto lastTime = std::chrono::high_resolution_clock::now();

    // 所有任务必须执行完成，线程池才可以回收所有线程资源
    while (true)
    {
        std::shared_ptr<Task> task;
        {
            // 先获取锁
            std::unique_lock<std::mutex> lock(taskQueMtx_);

            std::cout << "tid:" << std::this_thread::get_id()
                << "尝试获取任务..." << std::endl;

            // cached模式下，有可能已经创建了很多的线程，但是空闲时间超过60s，应该把多余的线程
            // 结束回收掉（超过initThreadSize_数量的线程要进行回收）
            // 当前时间 - 上一次线程执行的时间 > 60s

            // 每一秒中返回一次   怎么区分：超时返回？还是有任务待执行返回
            // 锁 + 双重判断

            while (taskQue_.size() == 0)
            {
                // 线程池要结束，回收线程资源
                if (!isPoolRunning_)
                {
                    threads_.erase(threadid);
					--curThreadSize_;
                    std::cout << "threadid:" << std::this_thread::get_id() << " exit!"
                        << std::endl;
                    exitCond_.notify_all(); // 通知所有线程退出
                    return;                 // 线程函数结束，线程结束
                }

                if (poolMode_ == PoolMode::MODE_CACHED)
                {
                    // 条件变量，超时返回了
                    if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                    {
                        // 判断当前线程的空闲时间是否超过60s
                        auto curTime = std::chrono::high_resolution_clock::now();
                        auto duration = std::chrono::duration_cast<std::chrono::seconds>(curTime - lastTime);
                        if (duration.count() > THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_)
                        {
                            // 开始回收当前线程
                            // 记录线程数量的相关变量的值修改
                            // 把线程对象从线程列表容器中删除   没有办法 threadFunc《=》thread对象
                            // threadid => thread对象 => 删除
                            // 线程池要结束，回收线程资源
                            threads_.erase(threadid);
                            curThreadSize_--;
                            idleThreadSize_--;

                            std::cout << "threadid:" << std::this_thread::get_id() << " exit!"
                                << std::endl;
                            return;
                        }
                    }
                }
                else
                {
                    // 等待notEmpty条件
                    notEmpty_.wait(lock);
                }

            }
            idleThreadSize_--;
            std::cout << "tid:" << std::this_thread::get_id()
                << "获取任务成功..." << std::endl;

            // 从任务队列种取一个任务出来
            task = taskQue_.front();
            taskQue_.pop();
            --taskSize_;

            // 如果依然有剩余任务，继续通知其它得线程执行任务
            if (taskQue_.size() > 0)
            {
                notEmpty_.notify_all();
            }

            // 取出一个任务，进行通知，通知可以继续提交生产任务
            notFull_.notify_all();
        }// 就应该把锁释放掉

        // 当前线程负责执行这个任务
        if (task != nullptr)
        {
            task->exec();
        }

        ++idleThreadSize_;
        lastTime = std::chrono::high_resolution_clock::now();   // 更新线程执行完任务的时间
    }
}


bool ThreadPool::checkRunningState() const
{
    return isPoolRunning_;
}

////////////////  线程方法实现
int Thread::generateId_ = 0;

// 线程构造
Thread::Thread(ThreadFunc func) : func_(func), threadId_(generateId_++)
{

}

// 线程析构
Thread::~Thread()
{

}

// 线程启动
void Thread::start()
{
    // 创建一个线程来执行一个线程函数 pthread_create
    std::thread t(func_, threadId_);
    t.detach(); // 分离线程，线程结束后，资源自动回收
}

int Thread::getId() const
{
    return threadId_;
}


/////////////////  Task方法实现
Task::Task() : result_(nullptr)
{

}

void Task::exec()
{
    if (result_ != nullptr)
    {
        result_->setVal(run()); // 这里发生多态调用
    }
}


void Task::setResult(std::shared_ptr<Result> res)
{
    result_ = res;
}

/////////////////   Result方法的实现
Result::Result(bool isValid) : isValid_(isValid)
{
    // std::cout << "Result::Result() called" << std::endl;
}

Any Result::get()   // 用户调用的
{
    if (!isValid_)
    {
        return "";
    }
    sem_.wait();     // 等待任务执行完，task任务如果没有执行完，这里会阻塞用户的线程
    return std::move(any_); // 返回任务的返回值
}

void Result::setVal(Any any)    // 谁调用的呢
{
    // 存储task的返回值
    this->any_ = std::move(any);
    sem_.post();    // 已经获取的任务的返回值，增加信号量资源
}
