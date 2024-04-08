#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include<queue>
#include<memory>
#include<atomic>
#include<mutex>
#include<condition_variable>
#include<functional>
#include<unordered_map>
#include<thread>
#include<future>


const int TASK_MAX_THRESHHOLD = 2;//INT32_MAX; //开源的线程池一般的最大阈值任务数量
const int THREAD_MAX_THRESHHOLD = 1024;//线程数量的上线阈值
const int THREAD_MAX_IDLE_TIME = 60; // 单位：秒

enum class  PoolMode //枚举，访问枚举项目加上类型
{
	MODE_FIXED, //固定数量的线程
	MODE_CACHED, //线程数量可动态增长
};

//线程类型
class Thread
{
public:
	//线程函数对象类型
	using ThreadFunc = std::function<void(int)>;

	//线程构造
	Thread(ThreadFunc func)
		: func_(func)
		, threadId_(generateId_++)
	{}
	//线程析构
	~Thread() = default;
	//启动线程
	void start()
	{
		// 创建一个线程来执行一个线程函数 pthread_create
		std::thread t(func_, threadId_);  // C++11来说 线程对象t  和线程函数func_
		t.detach(); // 设置分离线程   pthread_detach  pthread_t设置成分离线程
	}

	//获取线程id
	int getId()const
	{
		return threadId_;
	}
private:
	ThreadFunc func_;
	static int generateId_;
	int threadId_; //保存线程id
};

////////////////  线程方法实现
int Thread::generateId_ = 0;

//线程池类型
class ThreadPool
{
public:
	// 线程池构造
	ThreadPool()
		: initThreadSize_(0)
		, taskSize_(0)
		, idleThreadSize_(0)
		, curThreadSize_(0)
		, taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
		, threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
		, poolMode_(PoolMode::MODE_FIXED)
		, isPoolRunning_(false)
	{}

	// 线程池析构
	~ThreadPool()
	{
		isPoolRunning_ = false;

		// 等待线程池里面所有的线程返回  有两种状态：阻塞 & 正在执行任务中
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();
		exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
	}

	//设置线程池工作模式
	void setMode(PoolMode mode)
	{
		if (checkRunningState())
			return;
		poolMode_ = mode;
	}


	//设置task任务队列上线阈值
	void setTaskQueMaxThreshHold(int threshhold)
	{
		if (checkRunningState())
			return;
		taskQueMaxThreshHold_ = threshhold;
	}

	//设置线程池cached模式下，线程阈值
	void setThreadSizeThreshHold(int threshhold)
	{
		if (checkRunningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED)
		{
			threadSizeThreshHold_ = threshhold;
		}
	}

	//给线程池提交任务
	//使用可变参模版编程，让submitTask可以接收任意任务函数和任意数量的参数
	//pool.submitTask(sum1, 10, 20)  
	//返回值future<>
	template<typename Func,typename... Args> //引用折叠原理
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))> //decltype根据表达式的类型推导出返回的类型
	{
		//打包任务，放入任务队列里
		using RType = decltype(func(args...));
		//创建packaged对象，不带参数的绑定参数的函数
		auto task = std::make_shared<std::packaged_task<RType()>>(std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
		std::future<RType> result = task->get_future();

		// 获取锁
		std::unique_lock<std::mutex> lock(taskQueMtx_);

		// 线程的通信  等待任务队列有空余   wait   wait_for   wait_until
		// 用户提交任务，最长不能阻塞超过1s，否则判断提交任务失败，返回
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool { return taskQue_.size() < (size_t)taskQueMaxThreshHold_; }))
		{
			// 表示notFull_等待1s种，条件依然没有满足
			std::cerr << "Task Queue Is Full, Submit Task Fail." << std::endl;
			// auto一个task返回一个空的RType的future任务返回值的零值
			auto task = std::make_shared<std::packaged_task<RType()>>([]()->RType {return RType(); });//没有被执行
			(*task)();//解引用，访问task对象
			return task->get_future();
		}

		// 如果有空余，把任务放入任务队列中
		//taskQue_.emplace(sp);
		//using Task = std::function<void()>;
		taskQue_.emplace([task]() {(*task)(); });//封装一个任务，去执行该任务lambda表达式无法访问lambda外部变量
		taskSize_++;

		// 因为新放了任务，任务队列肯定不空了，在notEmpty_上进行通知，赶快分配线程执行任务
		notEmpty_.notify_all();

		// cached模式 任务处理比较紧急 场景：小而快的任务 需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程出来
		if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshHold_)
		{
			std::cout << ">>> Create New Thread..." << std::endl;

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

		// 返回任务的Result对象
		return result;
		// return task->getResult();
	}

	//开启线程池
	void start(int initThreadSize = std::thread::hardware_concurrency()) //hardware_concurrency当前CPU的核心数量
	{
		// 设置线程池的运行状态
		isPoolRunning_ = true;

		// 记录初始线程个数
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;

		// 创建线程对象
		for (int i = 0; i < initThreadSize_; i++)
		{
			// 创建thread线程对象的时候，把线程函数给到thread线程对象，placeholders参数占位符
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
			// threads_.emplace_back(std::move(ptr));
		}

		// 启动所有线程  std::vector<Thread*> threads_;
		for (int i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start(); // 需要去执行一个线程函数
			idleThreadSize_++;    // 记录初始空闲线程的数量
		}
	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator = (const ThreadPool&) = delete;

private:
	//定义线程函数
	void threadFunc(int threadid)
	{
		auto lastTime = std::chrono::high_resolution_clock().now();

		// 所有任务必须执行完成，线程池才可以回收所有线程资源
		for (;;)  //死锁区域3
		{
			Task task;
			{
				// 先获取锁
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "Tid:" << std::this_thread::get_id()
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
						threads_.erase(threadid); // std::this_thread::getid()
						std::cout << "threadid:" << std::this_thread::get_id() << " Exit ! "
							<< std::endl;
						exitCond_.notify_all();
						return; // 线程函数结束，线程结束
					}

					if (poolMode_ == PoolMode::MODE_CACHED)
					{
						// 条件变量，超时返回了
						if (std::cv_status::timeout ==
							notEmpty_.wait_for(lock, std::chrono::seconds(1)))
						{
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_)
							{
								// 开始回收当前线程
								// 记录线程数量的相关变量的值修改
								// 把线程对象从线程列表容器中删除   没有办法 threadFunc《=》thread对象
								// threadid => thread对象 => 删除
								threads_.erase(threadid); // std::this_thread::getid()，删除线程
								curThreadSize_--;
								idleThreadSize_--;

								std::cout << " Threadid:" << std::this_thread::get_id() << " Exit!"
									<< std::endl;
								return;
							}
						}
					}
					else
					{
						// 等待notEmpty条件
						notEmpty_.wait(lock); //死锁区域1
					}

					//线程池结束，回收线程资源
					//if (!isPoolRunning_)
					//{
					//	threads_.erase(threadid); // std::this_thread::getid()
					//	std::cout << " ThreadId : " << std::this_thread::get_id() << " Exit ! "
					//		<< std::endl;
					//	exitCond_.notify_all();
					//	return; // 结束线程函数，就是结束当前线程了!

					//}
				}
				idleThreadSize_--;

				std::cout << "Tid:" << std::this_thread::get_id()
					<< "获取任务成功..." << std::endl;

				// 从任务队列种取一个任务出来
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;

				// 如果依然有剩余任务，继续通知其它得线程执行任务
				if (taskQue_.size() > 0)
				{
					notEmpty_.notify_all();
				}

				// 取出一个任务，进行通知，通知可以继续提交生产任务
				notFull_.notify_all();
			} // 就应该把锁释放掉

			// 当前线程负责执行这个任务
			if (task != nullptr)
			{
				// task->run(); // 执行任务；把任务的返回值setVal方法给到Result
				//task->exec();   //死锁区域2
				task(); //执行function<void()>
			}

			idleThreadSize_++;
			lastTime = std::chrono::high_resolution_clock().now(); // 更新线程执行完任务的时间
		}
		//threads_.erase(threadid); // std::this_thread::getid()
		//std::cout << " ThreadId : " << std::this_thread::get_id() << " Exit ! "
		//	<< std::endl;
		//exitCond_.notify_all();
		//return; // 结束线程函数，就是结束当前线程了!
	}

	//检查Pool的运行状态
	bool checkRunningState() const
	{
		return isPoolRunning_;
	}

private:
	//std::vector<Thread*> threads_; //线程列表 ,该方法需要手动释放vector中的数据
	//std::vector<std::unique_ptr<Thread>>threads_;  //线程列表
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;  //线程列表,可以增减的map存储线程
	int initThreadSize_; //初始的线程数量,size_t 无符号整形
	std::atomic_uint curThreadSize_;  //记录当前线程池里面线程的总数量
	int threadSizeThreshHold_;  //线程数量上线阈值
	std::atomic_uint idleThreadSize_;//记录空闲线程的数量

	//Task任务 => 函数对象
	using Task = std::function<void()>;
	std::queue<Task> taskQue_; //任务队列,用于处理用户提交的临时任务，保证任务的生命周期，同时出完后能够自动释放
	std::atomic_uint taskSize_;  // 任务的数量 //uint因为不小于0
	int taskQueMaxThreshHold_; //任务队列数量上限的阈值

	std::mutex taskQueMtx_; //保证任务队列的线程安全
	//条件变量
	std::condition_variable notFull_; //表示任务队列不满
	std::condition_variable notEmpty_; // 表示任务队列不空
	std::condition_variable exitCond_; //等待线程资源全部回收

	PoolMode poolMode_; //用来记录当前线程池的工作模式
	std::atomic_bool isPoolRunning_;//表示当前线程池的启动状态
};

#endif