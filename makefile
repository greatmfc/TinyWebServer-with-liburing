CXX ?= g++

CDEBUGFLAGS += -g -fsanitize=address -luring

CXXFLAGS += -luring

objects = main.cpp ./timer/lst_timer.cpp ./http/http_conn.cpp ./log/log.cpp ./CGImysql/sql_connection_pool.cpp webserver.cpp config.cpp ./iorws/iorws.cpp

server: $(objects)
	$(CXX) $(objects) -o server -I./liburing/src/include/ -L./liburing/src/ $(CXXFLAGS) -lpthread -lmysqlclient 

test: $(objects)
	$(CXX) $(objects) -o test -I./liburing/src/include/ -L./liburing/src/ $(CDEBUGFLAGS) -lpthread -lmysqlclient 

clean:
	rm  -r server
