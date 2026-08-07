#include <connectionpool.h>
#include <coro/task.h>

int main() {
    connectionpool::Config cfg;
    cfg.host = "localhost";
    cfg.user = "root";
    cfg.password = "password";
    cfg.database = "test";

    connectionpool pool(cfg);
    coro::Task<connection*> task = pool.async_borrow();
    
}