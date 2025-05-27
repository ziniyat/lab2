#include <iostream>
#include <vector>
#include <queue>
#include <random>
#include <map>
#include <ctime>
#include <boost/thread.hpp>
#include <boost/interprocess/sync/interprocess_semaphore.hpp>
#include <boost/chrono.hpp>
#include <atomic>


class QuantumSimulator {
public:
    /*
    в классе
    - 4 квантовых процессора
    - Семафор на 4 одновременных задачи
    - Счетчики задач для каждого процессора
    - Генератор уникальных ID задач
     */
    QuantumSimulator() : 
        available_processors(4),
        task_semaphore(4),
        next_task_id(1)  // Начинаем нумерацию задач с 1
    {
        // Инициализация статусов процессоров и счетчиков задач
        for (int i = 0; i < 4; ++i) {
            processor_status[i] = true;  // Все процессоры исправны
            processor_task_count[i] = 0; // Начальное количество задач - 0
        }
    }

    /*
    Добавление новой задачи в систему
    Приоритет задачи (1 - высший)
    is_critical флаг критической задачи
    task_id номер задачи 
     */
    void add_task(int priority, bool is_critical, int task_id = -1) {
        boost::unique_lock<boost::mutex> lock(task_mutex);
        
        // Генерируем новый ID, если не указан
        int actual_id = (task_id == -1) ? next_task_id++ : task_id;
        
        // Добавляем задачу в приоритетную очередь
        tasks.push(Task{priority, is_critical, actual_id});
        
        lock.unlock();
        task_condition.notify_one();  // Уведомляем один ожидающий поток
    }

    /*
    Имитация сбоя процессора с перенаправлением его задач
     */
    void processor_failure(int processor_id) {
        boost::unique_lock<boost::mutex> lock(processor_mutex);
        
        // Проверяем, не вышел ли уже процессор из строя
        if (!processor_status[processor_id]) {
            std::cout << "Процессор " << processor_id << " уже неисправен.\n";
            return;
        }
        
        // Помечаем процессор как неисправный
        processor_status[processor_id] = false;
        
        // Запоминаем количество задач для перенаправления
        int tasks_to_redirect = processor_task_count[processor_id];
        processor_task_count[processor_id] = 0;
        
        lock.unlock();
        
        std::cout << "Процессор " << processor_id << " вышел из строя. "
                  << "Перенаправление " << tasks_to_redirect << " задач...\n";
        
        // Перенаправляем задачи обратно в общую очередь
        for (int i = 0; i < tasks_to_redirect; ++i) {
            // Генерируем новые параметры для перенаправленных задач
            add_task(1 + std::rand() % 5,    // Случайный приоритет
                    std::rand() % 10 == 0);  // 10% вероятность критической
        }
    }

    /*
    Имитация восстановления процессора
     */
    void processor_repair(int processor_id) {
        boost::unique_lock<boost::mutex> lock(processor_mutex);
        
        // Проверяем, не работает ли уже процессор
        if (processor_status[processor_id]) {
            std::cout << "Процессор " << processor_id << " уже работает.\n";
            return;
        }
        
        // Восстанавливаем процессор
        processor_status[processor_id] = true;
        std::cout << "Ремонт: Процессор " << processor_id << " восстановлен.\n";
        lock.unlock();
    }

    /*
    Запуск рабочих потоков
     */
    void start() {
        // Создаем 10 рабочих потоков
        for (int i = 0; i < 10; ++i) {
            threads.create_thread(boost::bind(&QuantumSimulator::worker_thread, this, i));
        }
    }

    /*
    Остановка всех рабочих потоков
     */
    void stop() {
        {
            boost::unique_lock<boost::mutex> lock(task_mutex);
            shutdown = true;  // Устанавливаем флаг завершения
        }
        task_condition.notify_all();  // Будим все потоки
        threads.join_all();           // Ожидаем завершения всех потоков
    }

private:
    /*
    Структура задачи для хранения в очередях
     */
    struct Task {
        int priority;       
        bool is_critical;   
        int task_id;        

        // Оператор сравнения для приоритетной очереди
        bool operator<(const Task& other) const {
            // Критические задачи имеют абсолютный приоритет
            if (is_critical != other.is_critical) {
                return !is_critical;
            }
            // Для задач равной важности сравниваем приоритеты
            return priority > other.priority;
        }
    };

    /*
    Функция рабочего потока
     */
    void worker_thread(int thread_id) {
        // Генераторы случайных чисел для потока
        std::mt19937 gen(std::time(0) + thread_id);
        std::bernoulli_distribution failure_dist(0.1);  // 10% вероятность сбоя
        
        while (!shutdown) {
            Task current_task;
            
            // Получаем задачу из общей очереди
            {
                boost::unique_lock<boost::mutex> lock(task_mutex);
                
                // Ожидаем появления задач или сигнала завершения
                while (tasks.empty() && !shutdown) {
                    task_condition.wait(lock);
                }
                
                // Проверяем, не пора ли завершать работу
                if (shutdown) break;
                
                // Берем задачу с наивысшим приоритетом
                current_task = tasks.top();
                tasks.pop();
            }

            // Захватываем слот в семафоре (получаем доступ к процессору)
            task_semaphore.wait();

            // С вероятностью 10% вызываем сбой случайного процессора
            if (failure_dist(gen)) {
                int processor_to_fail = std::rand() % 4;  // Случайный процессор 0-3
                processor_failure(processor_to_fail);
            }

            // Выбираем процессор для выполнения задачи
            int processor_id = -1;
            {
                boost::unique_lock<boost::mutex> lock(processor_mutex);
                std::vector<int> available_processors;
                
                // Собираем список исправных процессоров
                for (const auto& proc : processor_status) {
                    if (proc.second) {  // Если процессор исправен
                        available_processors.push_back(proc.first);
                    }
                }
                
                // Если есть доступные процессоры
                if (!available_processors.empty()) {
                    // Выбираем случайный процессор
                    std::uniform_int_distribution<> dist(0, available_processors.size() - 1);
                    processor_id = available_processors[dist(gen)];
                    
                    // Увеличиваем счетчик задач для выбранного процессора
                    processor_task_count[processor_id]++;
                }
            }

            // Если нет доступных процессоров
            if (processor_id == -1) {
                std::cout << "[ОЖИДАНИЕ] Нет доступных процессоров. Задача " 
                          << current_task.task_id << " (приоритет: " << current_task.priority 
                          << ", критическая: " << current_task.is_critical 
                          << ") возвращена в очередь.\n";
                
                // Возвращаем задачу в общую очередь
                {
                    boost::unique_lock<boost::mutex> lock(task_mutex);
                    tasks.push(current_task);
                }
                
                task_semaphore.post();  // Освобождаем слот
                boost::this_thread::sleep_for(boost::chrono::milliseconds(100));
                continue;
            }

            // Выводим информацию о выполняемой задаче
            std::cout << "Поток " << thread_id << " выполняет задачу " << current_task.task_id 
                      << " (приоритет: " << current_task.priority 
                      << ", критическая: " << current_task.is_critical 
                      << ") на процессоре " << processor_id << "\n";

            // Имитируем обработку задачи (случайное время 500-1500 мс)
            std::uniform_int_distribution<> work_dist(500, 1500);
            boost::this_thread::sleep_for(boost::chrono::milliseconds(work_dist(gen)));

            // После выполнения задачи уменьшаем счетчик задач процессора
            {
                boost::unique_lock<boost::mutex> lock(processor_mutex);
                if (processor_task_count[processor_id] > 0) {
                    processor_task_count[processor_id]--;
                }
            }

            // Освобождаем слот в семафоре
            task_semaphore.post();
        }
    }

private:
    // Семафор для ограничения одновременных задач
    boost::interprocess::interprocess_semaphore task_semaphore;
    
    // Приоритетная очередь задач
    std::priority_queue<Task> tasks;
    
    // Группа рабочих потоков
    boost::thread_group threads;
    
    // Мьютексы для синхронизации
    boost::mutex task_mutex;         // Для доступа к общей очереди задач
    boost::mutex processor_mutex;    // Для доступа к статусам процессоров
    
    // Условная переменная для ожидания задач
    boost::condition_variable task_condition;
    
    // Статусы процессоров (true - исправен, false - сломан)
    std::map<int, bool> processor_status;
    
    // Счетчики задач на каждом процессоре
    std::map<int, int> processor_task_count;
    
    // Флаг для остановки потоков
    std::atomic<bool> shutdown{false};
    
    // Счетчик доступных процессоров
    std::atomic<int> available_processors;
    
    // Счетчик для генерации уникальных ID задач
    std::atomic<int> next_task_id;
};

int main() {
    std::srand(std::time(0));  // Инициализация генератора случайных чисел
    
    QuantumSimulator simulator;
    std::cout << "Запуск программы" << std::endl;
    simulator.start();

    // Генераторы случайных значений для задач
    std::mt19937 gen(std::time(0));
    std::uniform_int_distribution<> priority_dist(1, 5);  // Приоритеты 1-5
    std::bernoulli_distribution critical_dist(0.1);       // 10% критических задач

    // Добавляем начальные задачи (40 задач с ID 1-40)
    for (int i = 0; i < 40; ++i) {
        int priority = priority_dist(gen);
        bool is_critical = critical_dist(gen);
        simulator.add_task(priority, is_critical, i + 1);
        boost::this_thread::sleep_for(boost::chrono::milliseconds(200));
    }

    // Периодически восстанавливаем все процессоры
    for (int i = 0; i < 2; ++i) {
        boost::this_thread::sleep_for(boost::chrono::seconds(4));
        std::cout << "\n Восстановление всех процессоров. \n";
        for (int j = 0; j < 4; ++j) {
            simulator.processor_repair(j);
        }
    }

    // Даем время на обработку оставшихся задач
    boost::this_thread::sleep_for(boost::chrono::seconds(5));
    
    std::cout << "\n Остановка" << std::endl;
    simulator.stop();
    
    std::cout << "Работа завершена." << std::endl;
    return 0;
}
