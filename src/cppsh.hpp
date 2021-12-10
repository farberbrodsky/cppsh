#ifndef _CPPSH
#define _CPPSH
#include <memory>
#include <ostream>
#include <string_view>
#include <unordered_map>
#include <initializer_list>

namespace cppsh {
    class command;

    // Pipes

    enum class __in_pipe_type {
        in_pipe_proc,
        in_pipe_fd,
        in_pipe_stream
    };

    class out_pipe;
    class in_pipe {
        friend command;
        out_pipe *input = NULL;
        __in_pipe_type type;

        union {
            struct {
                command *owner;
                int fd;
                int write_end_fd;
            } proc;  // in_pipe_proc
            int fd;  // in_pipe_fd
            struct {
                int memfd;
                std::ostream *os;
            } to_stream;  // in_pipe_stream
        } data;

        in_pipe(__in_pipe_type type);

    public:
        ~in_pipe();
        static in_pipe real_fd(int fd);
        static in_pipe to_stream(std::ostream &os);
    };


    enum class __out_pipe_type {
        out_pipe_proc,
        out_pipe_fd
    };

    class out_pipe {
        friend command;
        in_pipe *output = NULL;
        __out_pipe_type type;

        union {
            struct {
                command *owner;
                int fd;
                int read_end_fd;
            } proc;  // out_pipe_proc
            int fd;  // out_pipe_fd
        } data;

        out_pipe(__out_pipe_type type);

    public:
        ~out_pipe();
        static out_pipe real_fd(int fd);
    };


    // Command

    class command {
        std::unordered_map<int, std::unique_ptr<in_pipe>> in_pipes;
        std::unordered_map<int, std::unique_ptr<out_pipe>> out_pipes;

        int argc;
        char **argv;
        pid_t child_pid;
        bool run_once = false;  // used to check you don't run a command twice

    public:
        bool running = false;

        command(std::initializer_list<std::string_view> args);
        ~command();

        /// Returns an in pipe by file descriptor and creates an in pipe implicitly
        in_pipe &pipe_in_fd(int fd);
        /// Returns an out pipe by file descriptor and creates an out pipe implicitly
        out_pipe &pipe_out_fd(int fd);

        /// Creates an in pipe if it doesn't exist, and sets it to take input from an out pipe
        in_pipe &pipe_in_fd(int fd, out_pipe &src);
        /// Creates an out pipe if it doesn't exist, and sets it to give input to an in pipe
        out_pipe &pipe_out_fd(int fd, in_pipe &dst);

        void run();
        /// Returns exit status
        int wait();
    };


    // Exceptions

    class command_not_found : public std::runtime_error {
    public:
        command_not_found(char *name): std::runtime_error { std::string { "command not found: " } + name } {}
    };

    class command_not_running : public std::logic_error {
    public:
        command_not_running(): std::logic_error { "Waiting for command but command is not running" } {}
    };

    class command_already_run : public std::logic_error {
    public:
        command_already_run(): std::logic_error { "Command already ran once, it can't run again" } {}
    };


    class pipe_set_twice : public std::logic_error {
    public:
        pipe_set_twice(): std::logic_error { "Pipe was set twice" } {}
    };

    class pipe_not_set : public std::logic_error {
    public:
        pipe_not_set(): std::logic_error { "Pipe was not set" } {}
    };
}

#endif
