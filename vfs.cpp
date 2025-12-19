#define FUSE_USE_VERSION 35

#include <fuse3/fuse.h>
#include <iostream>
#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>
#include <pwd.h>
#include <sys/stat.h>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <pthread.h>

using namespace std;

const string VFS_PATH = "/opt/users";
static map<string, map<string, string>> vfs_data;

// Синхронизация данных с /etc/passwd
void sync_vfs_with_passwd() {
    vfs_data.clear();
    
    ifstream passwd_file("/etc/passwd");
    if (!passwd_file.is_open()) {
        cerr << "Cannot open /etc/passwd" << endl;
        return;
    }

    string line;
    while (getline(passwd_file, line)) {
        vector<string> fields;
        string field;
        stringstream ss(line);
        
        while (getline(ss, field, ':')) {
            fields.push_back(field);
        }
        
        if (fields.size() >= 7) {
            string username = fields[0];
            string uid = fields[2];
            string home = fields[5];
            string shell = fields[6];
            
            // Включаем всех пользователей с UID >= 1000 и root
            int uid_num = stoi(uid);
            if (uid_num == 0 || uid_num >= 1000) {
                // Более либеральная проверка shell
                if (shell != "/bin/false" && shell != "/usr/sbin/nologin") {
                    vfs_data[username]["id"] = uid;
                    vfs_data[username]["home"] = home;
                    vfs_data[username]["shell"] = shell;
                }
            }
        }
    }
    passwd_file.close();
}

// FUSE операции
static int vfs_getattr(const char* path, struct stat* st, struct fuse_file_info* fi) {
    (void) fi;
    memset(st, 0, sizeof(struct stat));
    
    time_t now = time(NULL);
    st->st_atime = st->st_mtime = st->st_ctime = now;

    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0755;
        st->st_uid = getuid();
        st->st_gid = getgid();
        return 0;
    }

    char username[256];
    char filename[256];

    if (sscanf(path, "/%255[^/]/%255[^/]", username, filename) == 2) {
        if (vfs_data.count(username) && 
            (strcmp(filename, "id") == 0 || 
             strcmp(filename, "home") == 0 || 
             strcmp(filename, "shell") == 0)) {
            
            st->st_mode = S_IFREG | 0644;
            st->st_uid = getuid();
            st->st_gid = getgid();
            st->st_size = vfs_data[username][filename].size();
            return 0;
        }
        return -ENOENT;
    }

    if (sscanf(path, "/%255[^/]", username) == 1) {
        if (vfs_data.count(username)) {
            st->st_mode = S_IFDIR | 0755;
            st->st_uid = getuid();
            st->st_gid = getgid();
            return 0;
        }
        return -ENOENT;
    }

    return -ENOENT;
}

static int vfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, 
                      off_t offset, struct fuse_file_info* fi, enum fuse_readdir_flags flags) {
    (void) offset;
    (void) fi;
    (void) flags;

    filler(buf, ".", NULL, 0, FUSE_FILL_DIR_PLUS);
    filler(buf, "..", NULL, 0, FUSE_FILL_DIR_PLUS);

    if (strcmp(path, "/") == 0) {
        for (const auto& user : vfs_data) {
            filler(buf, user.first.c_str(), NULL, 0, FUSE_FILL_DIR_PLUS);
        }
        return 0;
    }

    string username(path + 1);
    if (vfs_data.count(username)) {
        filler(buf, "id", NULL, 0, FUSE_FILL_DIR_PLUS);
        filler(buf, "home", NULL, 0, FUSE_FILL_DIR_PLUS);
        filler(buf, "shell", NULL, 0, FUSE_FILL_DIR_PLUS);
        return 0;
    }

    return -ENOENT;
}

static int vfs_read(const char* path, char* buf, size_t size, off_t offset, 
                   struct fuse_file_info* fi) {
    (void) fi;

    char username[256];
    char filename[256];

    if (sscanf(path, "/%255[^/]/%255[^/]", username, filename) != 2) {
        return -ENOENT;
    }

    if (!vfs_data.count(username) || !vfs_data[username].count(filename)) {
        return -ENOENT;
    }

    const string& content = vfs_data[username][filename];
    if ((size_t)offset >= content.size()) {
        return 0;
    }

    size_t len = min(size, content.size() - offset);
    memcpy(buf, content.c_str() + offset, len);
    return len;
}

static int vfs_mkdir(const char* path, mode_t mode) {
    (void) mode;

    char username[256];

    if (sscanf(path, "/%255[^/]", username) == 1) {
        if (vfs_data.count(username)) {
            return -EEXIST;
        }

        cout << "VFS: Adding user: " << username << endl;
        
        // Пробуем разные команды добавления пользователя
        string command = "useradd -m -s /bin/bash " + string(username) + " 2>/dev/null";
        int result = system(command.c_str());
        
        if (result != 0) {
            command = "adduser --disabled-password --gecos '' " + string(username) + " 2>/dev/null";
            result = system(command.c_str());
        }
        
        if (result == 0) {
            // Ждем и синхронизируем
            sync_vfs_with_passwd();
            cout << "User " << username << " added successfully" << endl;
            return 0;
        }
        
        cerr << "Failed to create user: " << username << endl;
        return -EIO;
    }
    return 0;
}

static int vfs_rmdir(const char* path) {
    char username[256];
    
    if (sscanf(path, "/%255[^/]", username) == 1) {
        if (strchr(path + 1, '/') == NULL) {
            if (!vfs_data.count(username)) {
                return -ENOENT;
            }

            cout << "VFS: Deleting user: " << username << endl;
            
            string command = "userdel -r " + string(username) + " 2>/dev/null";
            int result = system(command.c_str());
            
            if (result == 0) {
                vfs_data.erase(username);
                cout << "User " << username << " deleted successfully" << endl;
                return 0;
            }
            
            cerr << "Failed to delete user: " << username << endl;
            return -EIO;
        }
        return -EPERM;
    }
    return -EPERM;
}

static struct fuse_operations vfs_operations = {
    .getattr = vfs_getattr,
    .mkdir   = vfs_mkdir,
    .rmdir   = vfs_rmdir,
    .read    = vfs_read,
    .readdir = vfs_readdir,
};

// Функция для запуска FUSE в отдельном процессе
static void* fuse_thread(void* arg) {
    (void)arg;
    
    // Синхронизируем данные
    sync_vfs_with_passwd();
    
    // Создаем точку монтирования если не существует
    mkdir(VFS_PATH.c_str(), 0755);
    
    // Аргументы для fuse_main
    const char* fuse_argv[] = {
        "kubsh_vfs",
        VFS_PATH.c_str(),
        "-f",              // foreground mode
        "-s",              // single thread
        NULL
    };
    
    int fuse_argc = 3;
    
    cout << "Mounting VFS at: " << VFS_PATH << endl;
    int ret = fuse_main(fuse_argc, (char**)fuse_argv, &vfs_operations, NULL);
    cout << "FUSE exited with code: " << ret << endl;
    
    return NULL;
}

void initialize_vfs() {
    // Создаем точку монтирования
    mkdir(VFS_PATH.c_str(), 0755);
    
    // Запускаем FUSE в отдельном потоке
    pthread_t fuse_thread_id;
    if (pthread_create(&fuse_thread_id, NULL, fuse_thread, NULL) != 0) {
        cerr << "Failed to create FUSE thread" << endl;
        return;
    }
    
    cout << "VFS initialized at: " << VFS_PATH << endl;
}

void cleanup_vfs() {
    // Отмонтировать FUSE
    string command = "fusermount -u " + VFS_PATH + " 2>/dev/null || fusermount3 -u " + VFS_PATH + " 2>/dev/null || true";
    system(command.c_str());
}
