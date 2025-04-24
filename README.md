# ThreadPool
基于可变参数模板实现的线程池

```
g++ test.cpp threadpool.cpp  -o test -lpthread
```

分别提交了三次

1. 第一次在vs2022上能运行，但是在ubuntu上不能运行
2. 第二次在ununtu上能运行，但是在vs2022上不能运行
3. 第三次使用工厂函数，使得在两个平台上都能运行
4. 第四次使用延迟绑定，使得在两个平台都能运行，同时解决循环引用问题

之前的有些版本经过valgrind检测到了内存泄漏问题，说明某些地方还是没有处理好

现给出最终版，修复本质问题

核心问题：生命周期不匹配导致的悬空指针和使用已释放内存

当 main 函数执行到 {} 作用域的末尾时，栈上的 res1 对象会被销毁。但是，线程池中的 Task 对象（仍在任务队列中或被工作线程持有）内部保存的 result_ 指针仍然指向已经被销毁的 res1 对象曾经占据的内存地址。

随后，当线程池的工作线程从队列中取出这个 Task 并执行 Task::exec() 方法时，它会尝试通过 result_ 指针调用 result_->setVal(run())。此时 result_ 是一个悬空指针 (dangling pointer)，它指向的内存已经无效或可能已被其他数据覆盖。在这种情况下，对 *result_ 的任何访问（包括调用其成员函数 setVal，以及在 setVal 中访问 sem_ 等成员）都会导致 使用已释放内存 (use-after-free) 的未定义行为。

未定义行为可能表现为各种症状，包括：

1. 程序崩溃（这是比较常见和幸运的情况，因为它提示了问题）。

2. 看似正常运行，但在某些特定条件下崩溃。

3. 数据损坏。

4. 内存泄漏： 如果 Result 对象内部管理了其他堆上的资源（尽管您的 Result 类中没有直接管理堆内存，但 Semaphore 可能间接涉及），或者如果 Task 在使用悬空指针操作 Result 时导致某些清理逻辑没有正确执行，就可能发生内存泄漏。例如，如果 Semaphore 的析构函数依赖于正确访问 Result 对象的状态，而这种访问失败，Semaphore 内部管理的资源就可能没有被释放。Valgrind 检测到的内存泄漏很可能与这种使用已释放内存后导致的清理失败有关。

5. 信号量操作错误：在 Result::setVal 中调用的 sem_.post()，如果 sem_ 对象本身已经因 Result 被销毁而失效，也会导致问题。

所以：

Result 对象的生命周期不再绑定到 main 函数的栈作用域，而是由 std::shared_ptr 的引用计数管理。只要 Task 对象（在线程池中被处理期间）或者用户代码（通过 submitTask 返回的 shared_ptr）仍然持有指向 Result 对象的 shared_ptr，Result 对象就保证是存活的。

工作线程在执行 Task::exec() 时，通过 Task 内部持有的 std::shared_ptr<Result> 访问 Result 对象。由于这个 shared_ptr 保证了 Result 对象在此刻是有效的（它的引用计数至少为 1），所以访问 result_->setVal() 不再是使用已释放内存，而是安全的操作。Result 内部的 Semaphore 也因此在其有效的生命周期内被正确访问和操作。