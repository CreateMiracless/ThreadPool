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


const int TASK_MAX_THRESHHOLD = 2;//INT32_MAX; //��Դ���̳߳�һ��������ֵ��������
const int THREAD_MAX_THRESHHOLD = 1024;//�߳�������������ֵ
const int THREAD_MAX_IDLE_TIME = 60; // ��λ����

enum class  PoolMode //ö�٣�����ö����Ŀ��������
{
	MODE_FIXED, //�̶��������߳�
	MODE_CACHED, //�߳������ɶ�̬����
};

//�߳�����
class Thread
{
public:
	//�̺߳�����������
	using ThreadFunc = std::function<void(int)>;

	//�̹߳���
	Thread(ThreadFunc func)
		: func_(func)
		, threadId_(generateId_++)
	{}
	//�߳�����
	~Thread() = default;
	//�����߳�
	void start()
	{
		// ����һ���߳���ִ��һ���̺߳��� pthread_create
		std::thread t(func_, threadId_);  // C++11��˵ �̶߳���t  ���̺߳���func_
		t.detach(); // ���÷����߳�   pthread_detach  pthread_t���óɷ����߳�
	}

	//��ȡ�߳�id
	int getId()const
	{
		return threadId_;
	}
private:
	ThreadFunc func_;
	static int generateId_;
	int threadId_; //�����߳�id
};

////////////////  �̷߳���ʵ��
int Thread::generateId_ = 0;

//�̳߳�����
class ThreadPool
{
public:
	// �̳߳ع���
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

	// �̳߳�����
	~ThreadPool()
	{
		isPoolRunning_ = false;

		// �ȴ��̳߳��������е��̷߳���  ������״̬������ & ����ִ��������
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();
		exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
	}

	//�����̳߳ع���ģʽ
	void setMode(PoolMode mode)
	{
		if (checkRunningState())
			return;
		poolMode_ = mode;
	}


	//����task�������������ֵ
	void setTaskQueMaxThreshHold(int threshhold)
	{
		if (checkRunningState())
			return;
		taskQueMaxThreshHold_ = threshhold;
	}

	//�����̳߳�cachedģʽ�£��߳���ֵ
	void setThreadSizeThreshHold(int threshhold)
	{
		if (checkRunningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED)
		{
			threadSizeThreshHold_ = threshhold;
		}
	}

	//���̳߳��ύ����
	//ʹ�ÿɱ��ģ���̣���submitTask���Խ������������������������Ĳ���
	//pool.submitTask(sum1, 10, 20)  
	//����ֵfuture<>
	template<typename Func,typename... Args> //�����۵�ԭ��
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))> //decltype���ݱ��ʽ�������Ƶ������ص�����
	{
		//������񣬷������������
		using RType = decltype(func(args...));
		//����packaged���󣬲��������İ󶨲����ĺ���
		auto task = std::make_shared<std::packaged_task<RType()>>(std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
		std::future<RType> result = task->get_future();

		// ��ȡ��
		std::unique_lock<std::mutex> lock(taskQueMtx_);

		// �̵߳�ͨ��  �ȴ���������п���   wait   wait_for   wait_until
		// �û��ύ�����������������1s�������ж��ύ����ʧ�ܣ�����
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool { return taskQue_.size() < (size_t)taskQueMaxThreshHold_; }))
		{
			// ��ʾnotFull_�ȴ�1s�֣�������Ȼû������
			std::cerr << "Task Queue Is Full, Submit Task Fail." << std::endl;
			// autoһ��task����һ���յ�RType��future���񷵻�ֵ����ֵ
			auto task = std::make_shared<std::packaged_task<RType()>>([]()->RType {return RType(); });//û�б�ִ��
			(*task)();//�����ã�����task����
			return task->get_future();
		}

		// ����п��࣬������������������
		//taskQue_.emplace(sp);
		//using Task = std::function<void()>;
		taskQue_.emplace([task]() {(*task)(); });//��װһ������ȥִ�и�����lambda���ʽ�޷�����lambda�ⲿ����
		taskSize_++;

		// ��Ϊ�·�������������п϶������ˣ���notEmpty_�Ͻ���֪ͨ���Ͽ�����߳�ִ������
		notEmpty_.notify_all();

		// cachedģʽ ������ȽϽ��� ������С��������� ��Ҫ�������������Ϳ����̵߳��������ж��Ƿ���Ҫ�����µ��̳߳���
		if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshHold_)
		{
			std::cout << ">>> Create New Thread..." << std::endl;

			// �����µ��̶߳���
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
			// �����߳�
			threads_[threadId]->start();
			// �޸��̸߳�����صı���
			curThreadSize_++;
			idleThreadSize_++;
		}

		// ���������Result����
		return result;
		// return task->getResult();
	}

	//�����̳߳�
	void start(int initThreadSize = std::thread::hardware_concurrency()) //hardware_concurrency��ǰCPU�ĺ�������
	{
		// �����̳߳ص�����״̬
		isPoolRunning_ = true;

		// ��¼��ʼ�̸߳���
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;

		// �����̶߳���
		for (int i = 0; i < initThreadSize_; i++)
		{
			// ����thread�̶߳����ʱ�򣬰��̺߳�������thread�̶߳���placeholders����ռλ��
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
			// threads_.emplace_back(std::move(ptr));
		}

		// ���������߳�  std::vector<Thread*> threads_;
		for (int i = 0; i < initThreadSize_; i++)
		{
			threads_[i]->start(); // ��Ҫȥִ��һ���̺߳���
			idleThreadSize_++;    // ��¼��ʼ�����̵߳�����
		}
	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator = (const ThreadPool&) = delete;

private:
	//�����̺߳���
	void threadFunc(int threadid)
	{
		auto lastTime = std::chrono::high_resolution_clock().now();

		// �����������ִ����ɣ��̳߳زſ��Ի��������߳���Դ
		for (;;)  //��������3
		{
			Task task;
			{
				// �Ȼ�ȡ��
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "Tid:" << std::this_thread::get_id()
					<< "���Ի�ȡ����..." << std::endl;

				// cachedģʽ�£��п����Ѿ������˺ܶ���̣߳����ǿ���ʱ�䳬��60s��Ӧ�ðѶ�����߳�
				// �������յ�������initThreadSize_�������߳�Ҫ���л��գ�
				// ��ǰʱ�� - ��һ���߳�ִ�е�ʱ�� > 60s

				// ÿһ���з���һ��   ��ô���֣���ʱ���أ������������ִ�з���
				// �� + ˫���ж�
				while (taskQue_.size() == 0)
				{
					// �̳߳�Ҫ�����������߳���Դ
					if (!isPoolRunning_)
					{
						threads_.erase(threadid); // std::this_thread::getid()
						std::cout << "threadid:" << std::this_thread::get_id() << " Exit ! "
							<< std::endl;
						exitCond_.notify_all();
						return; // �̺߳����������߳̽���
					}

					if (poolMode_ == PoolMode::MODE_CACHED)
					{
						// ������������ʱ������
						if (std::cv_status::timeout ==
							notEmpty_.wait_for(lock, std::chrono::seconds(1)))
						{
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_)
							{
								// ��ʼ���յ�ǰ�߳�
								// ��¼�߳���������ر�����ֵ�޸�
								// ���̶߳�����߳��б�������ɾ��   û�а취 threadFunc��=��thread����
								// threadid => thread���� => ɾ��
								threads_.erase(threadid); // std::this_thread::getid()��ɾ���߳�
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
						// �ȴ�notEmpty����
						notEmpty_.wait(lock); //��������1
					}

					//�̳߳ؽ����������߳���Դ
					//if (!isPoolRunning_)
					//{
					//	threads_.erase(threadid); // std::this_thread::getid()
					//	std::cout << " ThreadId : " << std::this_thread::get_id() << " Exit ! "
					//		<< std::endl;
					//	exitCond_.notify_all();
					//	return; // �����̺߳��������ǽ�����ǰ�߳���!

					//}
				}
				idleThreadSize_--;

				std::cout << "Tid:" << std::this_thread::get_id()
					<< "��ȡ����ɹ�..." << std::endl;

				// �����������ȡһ���������
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;

				// �����Ȼ��ʣ�����񣬼���֪ͨ�������߳�ִ������
				if (taskQue_.size() > 0)
				{
					notEmpty_.notify_all();
				}

				// ȡ��һ�����񣬽���֪ͨ��֪ͨ���Լ����ύ��������
				notFull_.notify_all();
			} // ��Ӧ�ð����ͷŵ�

			// ��ǰ�̸߳���ִ���������
			if (task != nullptr)
			{
				// task->run(); // ִ�����񣻰�����ķ���ֵsetVal��������Result
				//task->exec();   //��������2
				task(); //ִ��function<void()>
			}

			idleThreadSize_++;
			lastTime = std::chrono::high_resolution_clock().now(); // �����߳�ִ���������ʱ��
		}
		//threads_.erase(threadid); // std::this_thread::getid()
		//std::cout << " ThreadId : " << std::this_thread::get_id() << " Exit ! "
		//	<< std::endl;
		//exitCond_.notify_all();
		//return; // �����̺߳��������ǽ�����ǰ�߳���!
	}

	//���Pool������״̬
	bool checkRunningState() const
	{
		return isPoolRunning_;
	}

private:
	//std::vector<Thread*> threads_; //�߳��б� ,�÷�����Ҫ�ֶ��ͷ�vector�е�����
	//std::vector<std::unique_ptr<Thread>>threads_;  //�߳��б�
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;  //�߳��б�,����������map�洢�߳�
	int initThreadSize_; //��ʼ���߳�����,size_t �޷�������
	std::atomic_uint curThreadSize_;  //��¼��ǰ�̳߳������̵߳�������
	int threadSizeThreshHold_;  //�߳�����������ֵ
	std::atomic_uint idleThreadSize_;//��¼�����̵߳�����

	//Task���� => ��������
	using Task = std::function<void()>;
	std::queue<Task> taskQue_; //�������,���ڴ����û��ύ����ʱ���񣬱�֤������������ڣ�ͬʱ������ܹ��Զ��ͷ�
	std::atomic_uint taskSize_;  // ��������� //uint��Ϊ��С��0
	int taskQueMaxThreshHold_; //��������������޵���ֵ

	std::mutex taskQueMtx_; //��֤������е��̰߳�ȫ
	//��������
	std::condition_variable notFull_; //��ʾ������в���
	std::condition_variable notEmpty_; // ��ʾ������в���
	std::condition_variable exitCond_; //�ȴ��߳���Դȫ������

	PoolMode poolMode_; //������¼��ǰ�̳߳صĹ���ģʽ
	std::atomic_bool isPoolRunning_;//��ʾ��ǰ�̳߳ص�����״̬
};

#endif