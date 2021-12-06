#include "cppsh.hpp"
#include <vector>
#include <iostream>  // debugging only
#include <filesystem>
#include <unordered_set>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

using cppsh::in_pipe;
using cppsh::out_pipe;
using cppsh::command;


in_pipe::in_pipe(cppsh::__in_pipe_type type)
:type { type }, data {} {}

out_pipe::out_pipe(cppsh::__out_pipe_type type)
:type { type }, data {} {}

in_pipe in_pipe::real_fd(int fd) {
    in_pipe res { cppsh::__in_pipe_type::in_pipe_fd };
    res.data.fd = fd;
    return res;
}

out_pipe out_pipe::real_fd(int fd) {
    out_pipe res { cppsh::__out_pipe_type::out_pipe_fd };
    res.data.fd = fd;
    return res;
}

out_pipe out_pipe::to_stream(std::ostream &os) {
    out_pipe res { cppsh::__out_pipe_type::out_pipe_stream };
    res.data.os = &os;
    return res;
}


in_pipe &command::pipe_in_fd(int fd) {
    auto &value = this->in_pipes[fd];
    if (value.get() != nullptr) {
        return *(value.get());
    } else {
        in_pipe *pipe = new in_pipe { cppsh::__in_pipe_type::in_pipe_proc };
        pipe->data.proc.fd = fd;
        pipe->data.proc.owner = this;
        value.reset(pipe);
        return *pipe;
    }
}

out_pipe &command::pipe_out_fd(int fd) {
    auto &value = this->out_pipes[fd];
    if (value.get() != nullptr) {
        return *(value.get());
    } else {
        out_pipe *pipe = new out_pipe { cppsh::__out_pipe_type::out_pipe_proc };
        pipe->data.proc.fd = fd;
        pipe->data.proc.owner = this;
        value.reset(pipe);
        return *pipe;
    }
}

// TODO throw exception if changing an existing input/output
in_pipe &command::pipe_in_fd(int fd, const out_pipe &src) {
    auto &pipe = this->pipe_in_fd(fd);
    pipe.input = &src;
    return pipe;
}

out_pipe &command::pipe_out_fd(int fd, const in_pipe &dst) {
    auto &pipe = this->pipe_out_fd(fd);
    pipe.output = &dst;
    return pipe;
}

command::command(std::initializer_list<std::string_view> args) {
    this->argc = args.size();
    this->argv = new char *[argc + 1];
    size_t i = 0;
    for (auto it = args.begin(); it != args.end(); ++it) {
        argv[i] = new char[it->size() + 1];
        it->copy(argv[i], it->size());
        i++;
    }
    argv[i] = NULL;
}

command::~command() {
    for (int i = 0; i < this->argc; i++) {
        delete[] this->argv[i];
    }

    delete[] this->argv;
}

// Heart of the library
void command::run() {
    // Create in and out pipes
    std::vector<std::pair<int, int>> set_fds;  // target fd, current fd
    std::unordered_set<int> dont_close;

    for (auto &pair : this->out_pipes) {
        int fd = pair.first;
        const in_pipe *out = pair.second->output;

        if (out->type == cppsh::__in_pipe_type::in_pipe_fd) {
            set_fds.push_back(std::make_pair(fd, out->data.fd));
            dont_close.emplace(out->data.fd);
        } else if (out->type == cppsh::__in_pipe_type::in_pipe_proc) {
            if (out->data.proc.owner->running) {
                // TODO do stuff
            } else {
                // TODO do stuff
            }
        }
    }

    // Create a pipe that is used for communicating errors
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) throw std::system_error(errno, std::system_category(), "couldn't create pipe");

    this->child_pid = fork();
    if (this->child_pid == 0) {
        using std::to_string;

        close(pipefd[0]);               // close the read end
        dont_close.emplace(pipefd[1]);  // keep the write end

        int max_fd = -1;  // may be used later for moving the communication pipe's fd

        // close all file descriptors other than dont_close
        std::vector<int> close_soon;  // needed because filesystem uses a file descriptor itself
        for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd/")) {
            int open_fd = stoi(entry.path().filename().string());
            if (open_fd > max_fd) max_fd = open_fd;

            if (dont_close.count(open_fd) == 0) {
                close_soon.push_back(open_fd);
            }
        }

        for (int fd : close_soon) {
            close(fd);
        }

        // move to target fds
        for (auto [target_fd, curr_fd] : set_fds) {
            if (curr_fd != target_fd) {
                if (target_fd == pipefd[1]) {
                    // move pipefd[1] somewhere else that isn't in use
                    int new_pipefd = dup3(pipefd[1], max_fd + 1, O_CLOEXEC);
                    if (new_pipefd == -1) {
                        // report this error
                        std::string msg = to_string(errno) + " dup3";
                        write(pipefd[1], msg.c_str(), msg.length());
                        exit(1);
                    } else {
                        pipefd[1] = new_pipefd;
                    }
                }
                if (dup2(curr_fd, target_fd) == -1) {
                    // report this error
                    std::string msg = to_string(errno) + " dup2";
                    write(pipefd[1], msg.c_str(), msg.length());
                    exit(1);
                };
                close(curr_fd);
            }
        }

        execv(this->argv[0], this->argv);
        // execv has failed, report this error
        std::string msg = to_string(errno) + " execve";
        write(pipefd[1], msg.c_str(), msg.length());
        exit(1);
    } else if (this->child_pid > 0) {
        close(pipefd[1]);  // close the write end
        char buf[512];
        size_t count = read(pipefd[0], buf, sizeof(buf));
        close(pipefd[0]);
        if (count == -1) {
            throw std::system_error(errno, std::system_category(), "reading from pipe");
        } else if (count != 0) {
            // throw the exception that was sent
            std::string msg { buf };

            int err = std::stoi(msg);
            msg = msg.substr(msg.find(' ') + 1);

            if (err == ENOENT && msg == "execve") {
                throw cppsh::command_not_found { this->argv[0] };
            }

            throw std::system_error(err, std::system_category(), msg);
        }
    } else {
        throw std::runtime_error { "Couldn't open subprocess" };
    }
}

int command::wait() {
    int wstatus;
    waitpid(this->child_pid, &wstatus, 0);
    return wstatus;
}

int main() {
    command c { "/usr/bin/cat", "/proc/version" };
    c.pipe_out_fd(1, in_pipe::real_fd(1));
    c.pipe_out_fd(2, in_pipe::real_fd(2));
    c.run();
    int w = c.wait();
    std::cout << "\n";
    if (WIFEXITED(w)) std::cout << "exited: " << WEXITSTATUS(w) << "\n";
    if (WIFSIGNALED(w)) std::cout << "signaled: " << WTERMSIG(w) << "\n";
    if (WCOREDUMP(w)) std::cout << "core dumped\n";
    if (WIFSTOPPED(w)) std::cout << "stopped: " << WSTOPSIG(w) << "\n";
    if (WIFCONTINUED(w)) std::cout << "continued\n";
    std::cout << std::flush;
    return 0;
}
