#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <iostream>
#include <vector>
#include <string.h>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>
#include <errno.h>

using namespace std;

#define EVENT_SIZE  ( sizeof (struct inotify_event) )
#define BUF_LEN     ( 1024 * ( EVENT_SIZE + 16 ) )
#define WATCH_FLAGS ( IN_CREATE | IN_DELETE | IN_MODIFY )

void writeToLog(string message)
{
  time_t t;
  time(&t);

  string time_event = ctime(&t);
  time_event.resize(time_event.size() - 1);

  ofstream fout("./chscan.log", ios_base::app);
  fout << time_event << " | " << message << "\n";

  fout.close();
}

//Функция возвращает по ссылке вектор со всеми директориями и поддерикториями, любой вложенности
static void list_dir(const char *dir_name, vector<string> &v) {

  //  Добавляем корневой каталог
  if (v.size() < 1) {
    v.push_back((const char *) dir_name);
  }

  DIR *d;

  /* Открываем директорию с именем "dir_name". */
  d = opendir(dir_name);

  /* Проверяем на возможность открытия */
  if (!d) {
    writeToLog("Cannot open directory: " + (string)dir_name);
    exit (EXIT_FAILURE);
  }
  while (1) {
    struct dirent *entry;
    const char *d_name;

    /* "Readdir" получает записи из "d". */
    entry = readdir(d);
    if (!entry) {
      /* Если больше нет записей выход из цикла */
      break;
    }
    d_name = entry->d_name;

    if (entry->d_type & DT_DIR) {

      /* Убедится что директория не "d" или родитель */
      if (strcmp(d_name, "..") != 0 && strcmp(d_name, ".") != 0) {
        int path_length;
        char path[PATH_MAX];

        path_length = snprintf(path, PATH_MAX, "%s/%s", dir_name, d_name);

        v.push_back(path);

        if (path_length >= PATH_MAX) {
          writeToLog("Path length has got too long.");
          exit(EXIT_FAILURE);
        }
        /* Рекурсивный вызов */
        list_dir(path, v);
      }
    }
  }
  /* После всех циклов инциализация закрытия */
  if (closedir(d)) {
    writeToLog("\"Could not close: " + (string)dir_name);
    exit(EXIT_FAILURE);
  }
}

bool dirOrNot(struct inotify_event *event, const string eventE, const string eventDir)
{
  if (event->mask & IN_ISDIR) {
    writeToLog("Directory | " + (string)event->name + " | " + (string)eventE + " | " + (string)eventDir);
    return true;
  }
  else {
    writeToLog("File | " + (string)event->name + " | " + (string)eventE + " | " + (string)eventDir);
  }
  return false;
}

void add_watches(int fd, vector<string> v, int * &wd)
{

  for(int i = 0; i < v.size(); i++) {
    wd[i] = inotify_add_watch(fd, v[i].c_str(), WATCH_FLAGS);
    if (wd[i] == -1)
      writeToLog("Couldn't add watch to the directory: " + v[i]);
    else
      writeToLog("Watching::" + v[i]);
  }
}

int main(int argc, char **argv) {

  pid_t pid;
  // Проверка на аргумент
  switch (argc) {
    case 1 :
    case 2 :
      printf("Usage: ./chscan [directory] [you mail]\n");
      return -1;
    case 3 :
      break;
    default:
      printf("Usage: ./chscan [directory] [you mail]\n");
      return -1;
  }

  pid = fork();

  if(pid < 0)
  {
    printf("Error: Start Daemon failed (%s)\n", strerror(errno));
    return -1;
  }
  else if(!pid)
  {
    // данный код уже выполняется в процессе потомка
    // разрешаем выставлять все биты прав на создаваемые файлы,
    // иначе у нас могут быть проблемы с правами доступа
    umask(0);

    // создаём новый сеанс, чтобы не зависеть от родителя
    setsid();

    // переходим в корень диска, если мы этого не сделаем, то могут быть проблемы.
    // к примеру с размантированием дисков
    // chdir("/");

    // закрываем дискрипторы ввода/вывода/ошибок, так как нам они больше не понадобятся
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    int fd;

    fd = inotify_init();

    if (fd < 0) {
      perror("inotify_init");
    }

//  Вектор типа строки, в котором по ссылке через функцию list_dir() передадут список директорий.
    vector<string> v;
    list_dir(argv[1], v);
    int *wd = new int(v.size());
    add_watches(fd, v, wd);

    int length, i = 0;
    char buffer[BUF_LEN];

    while (1) {

      length = read(fd, buffer, BUF_LEN);

      if (length < 0) {
        perror("read");
      }

      struct inotify_event *event = (struct inotify_event *) &buffer[0];

      if (event->len) {

        string eventD = "";
        for(int g = 0; g < v.size(); g++)
          if(event->wd == wd[g])
            eventD = v[g];

        switch (event->mask & WATCH_FLAGS) {
          case IN_MODIFY : {            
            dirOrNot(event, "IN_MODIFY", eventD);
            string Body = "File | " + (string)event->name + " | IN_MODIFY | " + (string)eventD;
            string sendEmail = "php -r \"mail('" + (string)argv[2] + "', 'IN_MODIFY', '" + Body + "');\"";
            system(sendEmail.c_str());
            break;
          }
          case IN_CREATE : {
            if (dirOrNot(event, "IN_CREATE", eventD)) {
              v.clear();
              list_dir(argv[1], v);
              wd = new int(v.size());
              add_watches(fd, v, wd);
            }
            break;
          }
          case IN_DELETE : {
            dirOrNot(event, "IN_DELETE", eventD);
            break;
          }
          default : {
            printf("UNKNOWN EVENT \"%X\" OCCURRED for file \"%s\" on WD #%i\n",
                   event->mask, event->name, event->wd);
            break;
          }
        }
        i += EVENT_SIZE + event->len;
      }
    }

    /* освобождение ресурсов*/
    ( void ) close( fd );
  }
  else { // если это родитель
    return 0;
  }
}