# io_uring_test
this is a  example of how to use io_uring

# prerequisite
sudo dnf install -y liburing-devel

# compile
gcc echo_server.c -o server -luring
gcc echo_client.c -o client -luring

