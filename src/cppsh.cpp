#include "cppsh.hpp"
#include <vector>
#include <iostream>    // debugging only
#include <filesystem>  // /proc/self/fd/
#include <unordered_set>
#include <fcntl.h>     // O_CLOEXEC
#include <unistd.h>    // pipe2
#include <sys/wait.h>  // waitpid
#include <sys/mman.h>  // memfd_create

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

in_pipe in_pipe::to_stream(std::ostream &os) {
    in_pipe res { cppsh::__in_pipe_type::in_pipe_stream };
    res.data.to_stream.memfd = memfd_create("pipe", 0);
    res.data.to_stream.os = &os;
    return res;
}

in_pipe::~in_pipe() {
    if (this->type == cppsh::__in_pipe_type::in_pipe_proc && this->data.proc.write_end_fd != -1) {
        close(this->data.proc.write_end_fd);
    } else if (this->type == cppsh::__in_pipe_type::in_pipe_stream) {
        close(this->data.to_stream.memfd);
    }
}

out_pipe::~out_pipe() {
    if (this->type == cppsh::__out_pipe_type::out_pipe_proc && this->data.proc.read_end_fd != -1) {
        close(this->data.proc.read_end_fd);
    }
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

in_pipe &command::pipe_in_fd(int fd, out_pipe &src) {
    in_pipe &pipe = this->pipe_in_fd(fd);
    if (pipe.input != nullptr || src.output != nullptr) {
        throw cppsh::pipe_set_twice {};
    }

    pipe.input = &src;
    src.output = &pipe;
    return pipe;
}

out_pipe &command::pipe_out_fd(int fd, in_pipe &dst) {
    out_pipe &pipe = this->pipe_out_fd(fd);
    if (pipe.output != nullptr || dst.input != nullptr) {
        throw cppsh::pipe_set_twice {};
    }

    dst.input = &pipe;
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
        argv[i][it->size()] = '\0';
        i++;
    }
    argv[i] = NULL;
}

command::~command() {
    for (int i = 0; i < this->argc; i++) {
        delete[] this->argv[i];
    }

    delete[] this->argv;

    if (this->running) {
        // kill the subprocess
        do {
            kill(this->child_pid, SIGKILL);
            this->wait();
        } while (this->running);
    }
}

static void write_errno_and_exit(int fd, std::string reason) {
    // used by child process
    std::string msg = std::to_string(errno) + " " + reason;
    write(fd, msg.c_str(), msg.length());
    exit(1);
}

// Heart of the library
void command::run() {
    if (this->run_once) {
        throw cppsh::command_already_run {};
    }

    // Create in and out pipes
    std::unordered_set<int> close_in_parent;   // pipe ends that the child will take over
    std::vector<std::pair<int, int>> set_fds;  // target fd, current fd
    std::unordered_set<int> dont_close;

    for (auto &pair : this->out_pipes) {
        int fd = pair.first;
        in_pipe *out = pair.second->output;
        if (out == nullptr) {
            throw cppsh::pipe_not_set {};
        }

        if (out->type == cppsh::__in_pipe_type::in_pipe_fd) {
            set_fds.emplace_back(fd, out->data.fd);
            dont_close.emplace(out->data.fd);
        } else if (out->type == cppsh::__in_pipe_type::in_pipe_stream) {
            set_fds.emplace_back(fd, out->data.to_stream.memfd);
            dont_close.emplace(out->data.to_stream.memfd);
        } else if (out->type == cppsh::__in_pipe_type::in_pipe_proc) {
            if (out->data.proc.owner->running) {
                // take the output's write end
                int write_end_fd = out->data.proc.write_end_fd;
                out->data.proc.write_end_fd = -1;

                set_fds.emplace_back(fd, write_end_fd);
                dont_close.emplace(write_end_fd);
                close_in_parent.emplace(write_end_fd);
            } else {
                // create a pipe and when the other process runs it will take our read end
                int pipefd[2];
                if (pipe2(pipefd, 0) != 0) throw std::system_error(errno, std::system_category(), "couldn't create pipe");
                pair.second->data.proc.read_end_fd = pipefd[0];

                // set the write end to be our desired file descriptor
                set_fds.emplace_back(fd, pipefd[1]);
                dont_close.emplace(pipefd[1]);
                close_in_parent.emplace(pipefd[1]);
            }
        }
    }
    for (auto &pair : this->in_pipes) {
        int fd = pair.first;
        out_pipe *in = pair.second->input;
        if (in == nullptr) {
            throw cppsh::pipe_not_set {};
        }

        if (in->type == cppsh::__out_pipe_type::out_pipe_fd) {
            set_fds.emplace_back(fd, in->data.fd);
            dont_close.emplace(in->data.fd);
        } else if (in->type == cppsh::__out_pipe_type::out_pipe_proc) {
            if (in->data.proc.owner->running) {
                // take the input's read end
                int read_end_fd = in->data.proc.read_end_fd;
                in->data.proc.read_end_fd = -1;

                set_fds.emplace_back(fd, read_end_fd);
                dont_close.emplace(read_end_fd);
                close_in_parent.emplace(read_end_fd);
            } else {
                // create a pipe and when the other process runs it will take our write end
                int pipefd[2];
                if (pipe2(pipefd, 0) != 0) throw std::system_error(errno, std::system_category(), "couldn't create pipe");
                pair.second->data.proc.write_end_fd = pipefd[1];

                // set the read end to be our desired file descriptor
                set_fds.emplace_back(fd, pipefd[0]);
                dont_close.emplace(pipefd[0]);
                close_in_parent.emplace(pipefd[0]);
            }
        }
    }

    // Create a pipe that is used for communicating errors
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) throw std::system_error(errno, std::system_category(), "couldn't create pipe");

    this->running = true;
    this->run_once = true;
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

        // make max_fd bigger than every file descriptor that should exist
        for (auto [target_fd, curr_fd] : set_fds) {
            if (target_fd > max_fd) max_fd = target_fd;
            if (curr_fd > max_fd) max_fd = curr_fd;
        }

        for (int fd : close_soon) {
            close(fd);
        }

        // move to target fds
        dont_close.clear();  // will store file descriptors that are mapped to themselves, e.g. 1 to 1
        close_soon.clear();  // will store file descriptors that have been dup'd to another place and aren't needed
        for (auto it = set_fds.begin(); it != set_fds.end(); ++it) {
            auto [target_fd, curr_fd] = *it;

            if (curr_fd != target_fd) {
                if (target_fd == pipefd[1]) {
                    // move pipefd[1] somewhere else that isn't in use
                    int new_pipefd = dup3(pipefd[1], ++max_fd, O_CLOEXEC);
                    if (new_pipefd == -1) write_errno_and_exit(pipefd[1], "dup3");

                    pipefd[1] = new_pipefd;
                }

                // move curr_fd somewhere else
                if (dup2(curr_fd, ++max_fd) == -1) write_errno_and_exit(pipefd[1], "dup2");
                close_soon.push_back(curr_fd);  // don't close now because it might get used too
                it->second = max_fd;
            } else {
                dont_close.emplace(curr_fd);
            }
        }

        // close everything in close_soon that isn't in dont_close
        for (int fd : close_soon) {
            if (dont_close.count(fd) == 0) {
                close(fd);
            }
        }

        // duplicate fds to their targets
        for (auto [target_fd, curr_fd] : set_fds) {
            if (dup2(curr_fd, target_fd) == -1) write_errno_and_exit(pipefd[1], "dup2");
            if (dont_close.count(curr_fd) == 0) close(curr_fd);
        }

        execv(this->argv[0], this->argv);
        // execv has failed, report this error
        write_errno_and_exit(pipefd[1], "execve");

    } else if (this->child_pid > 0) {
        close(pipefd[1]);  // close the write end

        for (int fd : close_in_parent) {
            close(fd);
        }

        char buf[512];
        size_t count = read(pipefd[0], buf, sizeof(buf));
        close(pipefd[0]);
        if (count == -1) {
            throw std::system_error(errno, std::system_category(), "reading from pipe");
        } else if (count != 0) {
            // throw the exception that was sent
            std::string msg { buf, count };

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
    if (this->running) {
        int wstatus;
        waitpid(this->child_pid, &wstatus, 0);
        if (WIFEXITED(wstatus) || WIFSIGNALED(wstatus) || WIFSTOPPED(wstatus)) {
            this->running = false;
        }

        // read from memfds into streams
        // they are closed by their destructors
        char buf[4096];
        for (auto &[fd, pipe_ptr] : this->out_pipes) {
            in_pipe *output = pipe_ptr->output;
            if (output->type == cppsh::__in_pipe_type::in_pipe_stream) {
                int memfd = output->data.to_stream.memfd;
                lseek(memfd, 0, SEEK_SET);

                size_t count;
                while ((count = read(memfd, buf, sizeof(buf))) > 0) {
                    *(output->data.to_stream.os) << std::string_view { buf, count };
                }
            }
        }

        return wstatus;
    } else {
        throw command_not_running {};
    }
}

int main() {
    for (int i = 0; i < 10; i++) {
        std::stringstream ss {};
        in_pipe store_to_s = in_pipe::to_stream(ss);

        command cat { "/usr/bin/echo", "-e", "abc\nworld\nthis\nworks\nhello world\nasdf" };
        command grep { "/usr/bin/grep", "hello" };

        cat.pipe_out_fd(1, grep.pipe_in_fd(0)); // equivalent: grep.pipe_in_fd(0, cat.pipe_out_fd(1));
        grep.pipe_out_fd(1, store_to_s);
        grep.run();
        cat.run();

        while (cat.running) cat.wait();
        while (grep.running) grep.wait();

        std::cout << "Output of commands is: " << ss.str() << std::flush;
    }

    return 0;
}
