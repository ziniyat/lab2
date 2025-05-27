
#include <iostream>
#include <queue>
#include <vector>
#include <random>
#include <atomic>
#include <boost/thread.hpp>
#include <boost/interprocess/sync/interprocess_semaphore.hpp>
#include <boost/chrono.hpp>

class EnergyMonitorSystem {
public:
    /*
    в классе системы мониторинга
    - Начальная нагрузка (0%)
    - Максимальная нагрузка (100%)
    - Базовые обработчики (2 шт)
    - Семафор для контроля обработчиков
     */
    EnergyMonitorSystem() : 
        current_load(0),
        max_load(100),
        base_handlers(2),
        additional_handlers(0),
        data_semaphore(2) // Инициализация семафора с 2 обработчиками
    {
        srand(time(0));
    }

    /*
    priority Приоритет данных (1 - наивысший)
    is_critical Флаг критически важных данных
     */
    void add_data_packet(int priority, bool is_critical, int station_id) {
        boost::unique_lock<boost::mutex> lock(data_mutex);
        data_packets.push(DataPacket{priority, is_critical, station_id});
        lock.unlock();
        data_condition.notify_one(); // Уведомляем сервер о новых данных
    }

    /*
    Запуск системы мониторинга
    Создаем
    - 1 поток для серверного обработчика
    - 10 потоков для станций мониторинга
     */
    void start() {
        server_thread = boost::thread(&EnergyMonitorSystem::server_handler, this);
        
        for (int i = 0; i < 10; ++i) {
            station_threads.create_thread(boost::bind(&EnergyMonitorSystem::station_thread, this, i));
        }
    }


  //Остановка системы мониторинга

    void stop() {
        {
            boost::unique_lock<boost::mutex> lock(data_mutex);
            shutdown = true;
        }
        data_condition.notify_all(); // Будим все ожидающие потоки
        station_threads.join_all();  // Ожидаем завершения станций
        server_thread.join();        // Ожидаем завершения сервера
    }

    /*
    Имитация аварийной ситуации
    Включает режим, при котором низкоприоритетные данные отбрасываются
     */
    void simulate_emergency() {
        boost::unique_lock<boost::mutex> lock(data_mutex);
        emergency_mode = true;
        std::cout << "\n АВАРИЯ. Включен аварийный режим. Низкоприоритетные данные будут отбрасываться.\n";
        lock.unlock();
    }

private:
    /*
    Структура пакета данных
    -Приоритет (1-5, где 1 - наивысший)
    -Флаг критичности
    -Идентификатор станции-отправителя
     */
    struct DataPacket {
        int priority;
        bool is_critical;
        int station_id;

        // Оператор сравнения для приоритетной очереди
        bool operator<(const DataPacket& other) const {
            // В аварийном режиме критические данные имеют абсолютный приоритет
            if (is_critical != other.is_critical) {
                return !is_critical;
            }
            // Для данных одинаковой важности сравниваем приоритеты
            return priority > other.priority;
        }
    };

    /*
    Поток станции мониторинга
     */
    void station_thread(int station_id) {
        std::mt19937 gen(time(0) + station_id);
        std::uniform_int_distribution<> priority_dist(1, 5);  // Приоритеты 1-5
        std::bernoulli_distribution critical_dist(0.15);      // 15% критических данных
        std::exponential_distribution<> interval_dist(1.0);   // Интервалы между отправками

        while (!shutdown) {
            // Генерируем пакет данных
            int priority = priority_dist(gen);
            bool is_critical = critical_dist(gen);
            
            // Отправляем данные на сервер
            add_data_packet(priority, is_critical, station_id);
            
            // Имитируем работу станции (случайный интервал)
            double interval = interval_dist(gen);
            boost::this_thread::sleep_for(boost::chrono::milliseconds(static_cast<int>(interval * 1000)));
        }
    }

    /*
    Основной обработчик сервера
    Обрабатывает данные с учетом:
    - Текущей нагрузки
    - Аварийного режима
    - Приоритетов данных
     */
    void server_handler() {
        while (!shutdown) {
            DataPacket packet;
            {
                boost::unique_lock<boost::mutex> lock(data_mutex);
                // Ожидаем данные или сигнал завершения
                while (data_packets.empty() && !shutdown) {
                    data_condition.wait(lock);
                }
                if (shutdown) break;

                // Берем пакет с наивысшим приоритетом
                packet = data_packets.top();
                data_packets.pop();
            }

            // Захватываем обработчик через семафор
            data_semaphore.wait();

            // Проверяем текущую нагрузку
            int load = current_load.load();
            
            // При высокой нагрузке добавляем обработчики
            if (load > 80 && additional_handlers < 3) {
                boost::unique_lock<boost::mutex> lock(handler_mutex);
                additional_handlers++;
                data_semaphore.post(); // Добавляем новый обработчик
                std::cout << "Нагрузка " << load << "%. Включен дополнительный обработчик. Всего: " 
                          << (base_handlers + additional_handlers) << "\n";
            }

            // В аварийном режиме проверяем приоритет
            if (emergency_mode) {
                // Отбрасываем низкоприоритетные некритические данные
                if (packet.priority > 3 && !packet.is_critical) {
                    std::cout << "АВАРИЯ. Отброшен пакет от станции " << packet.station_id 
                              << " (приоритет: " << packet.priority << ")\n";
                    data_semaphore.post();
                    continue; // Пропускаем обработку этого пакета
                }
                
                // Для критических данных уменьшаем интервал обработки
                if (packet.is_critical) {
                    std::cout << "АВАРИЯ. Срочная обработка критического пакета от станции " 
                              << packet.station_id << "\n";
                }
            }

            // Обработка данных (время зависит от нагрузки)
            std::cout << "Обработка пакета от станции " << packet.station_id 
                      << " (приоритет: " << packet.priority 
                      << ", критический: " << packet.is_critical << ")\n";

            // Имитация обработки (чем выше нагрузка, тем дольше обработка)
            int processing_time = 100 + (rand() % 400) * (current_load / 100.0);
            boost::this_thread::sleep_for(boost::chrono::milliseconds(processing_time));

            // Обновляем нагрузку
            int new_load = load + (processing_time / 10);
            if (new_load > max_load) new_load = max_load;
            current_load.store(new_load);

            // При низкой нагрузке отключаем дополнительные обработчики
            if (new_load < 50 && additional_handlers > 0) {
                boost::unique_lock<boost::mutex> lock(handler_mutex);
                additional_handlers--;
                data_semaphore.wait(); // Уменьшаем количество обработчиков
                std::cout << "Нагрузка " << new_load << "%. Отключен обработчик. Всего: " 
                          << (base_handlers + additional_handlers) << "\n";
            }

            data_semaphore.post(); // Освобождаем обработчик
        }
    }

private:
    // Очередь данных с приоритетом
    std::priority_queue<DataPacket> data_packets;
    
    // Потоки станций мониторинга и сервера
    boost::thread_group station_threads;
    boost::thread server_thread;
    
    // Синхронизация
    boost::mutex data_mutex;        // Для доступа к очереди данных
    boost::mutex handler_mutex;     // Для управления обработчиками
    boost::condition_variable data_condition; // Для ожидания данных
    boost::interprocess::interprocess_semaphore data_semaphore; // Контроль обработчиков
    
    // Состояние системы
    std::atomic<int> current_load;  // Текущая нагрузка (0-100%)
    std::atomic<int> additional_handlers; // Дополнительные обработчики
    std::atomic<bool> shutdown{false};    // Флаг завершения работы
    std::atomic<bool> emergency_mode{false}; // Аварийный режим
    
    // Константы
    const int max_load;       // Максимальная нагрузка (100%)
    const int base_handlers;  // Базовое количество обработчиков (2)
};

int main() {
    EnergyMonitorSystem system;
    std::cout << "Запуск системы мониторинга энергосети" << std::endl;
    system.start();

    // Имитация нормальной работы (5 секунд)
    boost::this_thread::sleep_for(boost::chrono::seconds(5));
    
    // Имитация аварии
    system.simulate_emergency();
    
    // Продолжаем работу в аварийном режиме (10 секунд)
    boost::this_thread::sleep_for(boost::chrono::seconds(10));
    
    // Завершение работы
    std::cout << "\n Остановка системы мониторинга" << std::endl;
    system.stop();
    
    return 0;
}
