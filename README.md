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