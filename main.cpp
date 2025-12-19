// база
#include <iostream>
#include <fstream>
#include <string>
// бинарники
#include <vector>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
// сигналы
#include <csignal>
#include <cstring>
// диски
#include <fcntl.h>
#include <sys/stat.h>
#include <cstdint>
// vfs
#include "vfs.h"

using namespace std;

// Флаг SIGHUP
volatile sig_atomic_t got_sighup = 0;

// Обработчик SIGHUP
void sighup_handler(int sig) {
  got_sighup = 1; 
}

// Функция для получения текстового описания типа раздела
string get_partition_type_description(uint8_t type) {
    switch(type) {
        case 0x00: return "Empty";
        case 0xEE: return "GPT Protective";
        case 0xEF: return "EFI System";
        case 0x07: return "NTFS/HPFS";
        case 0x0B: return "FAT32 (CHS)";
        case 0x0C: return "FAT32 (LBA)";
        case 0x05: return "Extended (CHS)";
        case 0x0F: return "Extended (LBA)";
        case 0x82: return "Linux Swap";
        case 0x83: return "Linux";
        case 0x8E: return "Linux LVM";
        default:   return "Unknown";
    }
}

// Улучшенная функция для чтения и анализа MBR с ручным парсингом
void list_partitions_mbr(const string& disk_path) {
    // Открываем диск в режиме только для чтения
    int fd = open(disk_path.c_str(), O_RDONLY);
    if (fd == -1) {
        cout << "Cannot open device: " << disk_path << " (errno: " << errno << ")" << endl;
        perror("open");
      return;
    }

    // Читаем весь MBR сектор (512 байт) в буфер
    unsigned char buffer[512];
    ssize_t bytes_read = read(fd, buffer, sizeof(buffer));
    close(fd);

    // Проверяем что прочитали полный MBR
    if (bytes_read != sizeof(buffer)) {
        cerr << "Error reading MBR from: " << disk_path << " (read " << bytes_read << " bytes)" << endl;
        return;
    }

    // Проверяем сигнатуру MBR вручную (0x55AA в конце)
    if (buffer[510] != 0x55 || buffer[511] != 0xAA) {
        cerr << "Invalid MBR signature on: " << disk_path << endl;
        cerr << "Got signature: 0x" << hex << (int)buffer[511] << (int)buffer[510] << dec << endl;
        return;
    }

    cout << "Disk analysis for: " << disk_path << endl;
    cout << "Partition table:" << endl;

    bool bootable_found = false;
    bool is_gpt_protective = false;

    // Анализируем все 4 записи разделов (смещение 0x1BE = 446)
    const int PARTITION_TABLE_OFFSET = 0x1BE;
    
    for (int i = 0; i < 4; i++) {
        int offset = PARTITION_TABLE_OFFSET + i * 16;
        
        // Извлекаем данные из буфера вручную
        uint8_t status = buffer[offset];
        uint8_t type = buffer[offset + 4];
        
        // Извлекаем LBA начальный сектор (little-endian)
        uint32_t lba_start = 
            (buffer[offset + 11] << 24) | 
            (buffer[offset + 10] << 16) | 
            (buffer[offset + 9] << 8) | 
            buffer[offset + 8];
            
        // Извлекаем количество секторов (little-endian)
        uint32_t sector_count = 
            (buffer[offset + 15] << 24) | 
            (buffer[offset + 14] << 16) | 
            (buffer[offset + 13] << 8) | 
            buffer[offset + 12];

        cout << "Partition " << (i + 1) << ": ";
        
        // Проверяем загрузочный флаг
        if (status == 0x80) {
            cout << "Bootable, ";
            bootable_found = true;
        } else if (status == 0x00) {
            cout << "Non-bootable, ";
        } else {
            cout << "Unknown status (0x" << hex << (int)status << dec << "), ";
        }

        // Определяем тип раздела с текстовым описанием
        cout << "Type: 0x" << hex << (int)type << dec;
        cout << " (" << get_partition_type_description(type) << ")";

        // Проверяем GPT protective partition
        if (type == 0xEE) {
            is_gpt_protective = true;
        }

        // Выводим размер если раздел не пустой
        if (type != 0x00 && sector_count > 0) {
            uint64_t size_bytes = (uint64_t)sector_count * 512;
            if (size_bytes >= 1024 * 1024 * 1024) {
                cout << ", Size: " << (size_bytes / (1024.0 * 1024 * 1024)) << " GB";
            } else {
                cout << ", Size: " << (size_bytes / (1024.0 * 1024)) << " MB";
            }
            
            // Дополнительная информация: начальный сектор
            cout << ", Start LBA: " << lba_start;
        }
        
        cout << endl;
    }

    // Определяем тип таблицы разделов
    if (is_gpt_protective) {
        cout << "This disk uses GPT partitioning (protective MBR detected)" << endl;
    } else {
        cout << "This disk uses MBR partitioning" << endl;
    }

    if (!bootable_found) {
        cout << "No bootable partitions found" << endl;
    }
}

// debug
void handle_debug(string input) {
  input = input.substr(5); // -"debug"
  
  // -пробелы после debug
  while (!input.empty() && input[0] == ' ') {
    input.erase(0, 1);
  }
  
  // -кавычки
  if (!input.empty()) {
    if ((input[0] == '"' && input[input.length()-1] == '"') || 
        (input[0] == '\'' && input[input.length()-1] == '\'')) {
      input = input.substr(1, input.length()-2); 
    }
  }
  cout << input << '\n';
}

// Окружение
void handle_env(string input) {
  size_t pos = input.find("\\e") + 3; // -"\e "

  if (pos < input.length()) {  // Чек есть ли аргументы после команды
    string var_name = input.substr(pos);  // Имя переменной
    
    // -'$' если в начале имени
    if (!var_name.empty() && var_name[0] == '$') 
      var_name = var_name.substr(1);  // - $
    
    // Значение переменной окружения
    const char* env_value = getenv(var_name.c_str());
    if (env_value != nullptr) {  // Если переменная существует
      string value = env_value;  // string для удобства
      size_t start = 0;  // Нач поз для substring
      size_t end = value.find(':');  // Первый разделитель
      
      // Разделение по ':' и вывод
      while (end != string::npos) {  // Пока есть разделители
        cout << value.substr(start, end - start) << '\n';  // Часть до разделителя
        start = end + 1;  // Сдвиг начала за разделитель
        end = value.find(':', start);  // Следующий разделитель
      }
      // Оставшаяся часть после последнего разделителя
      cout << value.substr(start) << '\n';
    } else {
      // Переменная не найдена
      cout << "Environment variable '" << var_name << "' not found" << '\n';
    }
  } else {
    // Неправильный формат команды
    cout << "Usage: \\e $VARIABLE" << '\n';
  }
}

// Бинарники
void handle_external_command(string input) {
  // Дочерний процесс
  pid_t pid = fork();
  
  if (pid == 0) {
    // Команда
    
    // Разбиение на аргументы
    vector<string> args;
    stringstream ss(input);
    string token;
    
    while (ss >> token) {
      args.push_back(token);
    }
    
    // Аргументы для execvp
    vector<char*> argv;
    for (auto& arg : args) {  // Прогон по аргументам
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr); // Конец нуллом
    
    // Попытка выполнения
    execvp(argv[0], argv.data());
    
    // Если дошли сюда - команда не найдена
    cout << input << ": command not found\n";
    exit(1);
    
  } else if (pid > 0) {
    // Ждем дочь
    int status;
    waitpid(pid, &status, 0);
  } else {
    cerr << "Failed to create process" << '\n';
  }
}

// Сигналы
void setup_signal_handler() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));  // Обнуление структуры
  sa.sa_handler = sighup_handler;  // Функция-обработчик
  sa.sa_flags = SA_RESTART;  // Флаг для перезапуска после сигнала
  sigaction(SIGHUP, &sa, NULL);  // Регистр обработчика
}

int main() {
  cout << unitbuf;
  cerr << unitbuf;

  initialize_vfs();

  // Настройка обработчика
  setup_signal_handler();

  string hist = "kubsh_history.txt";
  ofstream F(hist, ios::app);

  cerr << "$ ";

  string input;
  while (getline(cin, input)) {

   if (got_sighup) {
        cout << "Configuration reloaded" << endl; 
        got_sighup = 0;
        cerr << "$ ";
        continue;
    }
        
    // Безопасное удаление начальных пробелов
    while (!input.empty() && input[0] == ' ') {
      input.erase(0, 1);
    }

    F << '$' << input << '\n';  
    F.flush(); 
    
    if (input == "\\q") 
      break;
    
    if (input.empty()) {
      cerr << "$ ";
      continue;
    }
    
    // Обработка debug
    if (input.find("debug") == 0) {
      handle_debug(input);
    }
    // Переменные окружения
    else if (input.find("\\e") == 0) {
      handle_env(input);
    }
    // Анализ диска
    else if (input.find("\\l") == 0) {
      if (input.length() > 3) {
        string device_path = input.substr(3); // -"\l "
        
        // Удаляем пробелы после команды
        while (!device_path.empty() && device_path[0] == ' ') {
          device_path.erase(0, 1);
        }
        
        if (!device_path.empty()) {
          list_partitions_mbr(device_path);
        } else {
          cout << "Usage: \\l /dev/device" << '\n';
        }
      } else {
        cout << "Usage: \\l /dev/device" << '\n';
      }
    }
    // Обработка бинарников
    else {
      handle_external_command(input);
    }

    cerr << "$ ";
  }
  cleanup_vfs();
}
