#include <iostream>
#include <functional>
#include <thread>
#include <future>
#include <chrono>
using namespace std;

#include"ThreadPoolOpen.h"

/*
让线程池提交任务更加方便
1. pool.submitTask(sum1, 10, 20)
   pool.submitTask(sum2, 1, 2, 3)
   submitTask:使用可变参模版类型编写

2.自己制作Result以及相关的类型，代码多
  C++11  有线程库 thread  在Windows下就调用Windows的系统API ，在Linux下就调用Linux的系统API，
                  packaged_task(function函数对象) async是更强大的接口，直接获取返回值
		  使Future来代替Result来节省线程池代码

*/

int sum1(int a, int b)
{
	this_thread::sleep_for(chrono::seconds(2));
	//如果该任务耗时多，那么用户在调用res.get()时会阻塞,直到任务执行完成
	return a + b;
}
int sum2(int a, int b, int c)
{
	this_thread::sleep_for(chrono::seconds(2));
	return a + b + c;
}
int main()
{
	ThreadPool pool;
	//pool.setMode(PoolMode::MODE_CACHED);
	pool.start(2);

	future<int> r1 = pool.submitTask(sum1, 1, 2);
	future<int> r2 = pool.submitTask(sum2, 1, 2, 3);
	future<int> r3 = pool.submitTask(
		[](int b, int e)->int 
		{
			int sum = 0; 
			for (int i = b; i <= e; i++)
				sum += i;
			return sum; 
		}, 1, 100);
	future<int> r4 = pool.submitTask(
		[](int b, int e)->int
		{
			int sum = 0;
			for (int i = b; i <= e; i++)
				sum += i;
			return sum;
		}, 1, 100);
	future<int> r5 = pool.submitTask(
		[](int b, int e)->int
		{
			int sum = 0;
			for (int i = b; i <= e; i++)
				sum += i;
			return sum;
		}, 1, 100);

	//future<int> r4 = pool.submitTask(sum1, 1, 2);

	cout << r1.get() << endl;
	cout << r2.get() << endl;
	cout << r3.get() << endl;
	cout << r4.get() << endl;
	cout << r5.get() << endl;
	//cout << r4.get() << endl;
	//packaged_task<int(int, int)> task(sum1);
	//// Future => Result
	//future<int> res = task.get_future();
	////task(10, 20);

	//thread t(std::move(task), 10, 20);
	//t.detach();

	//cout << res.get() << endl;
	/*thread t1(sum1, 10, 20);
	thread t2(sum2, 1, 2, 3);

	t1.join();
	t2.join();*/

}