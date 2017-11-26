s="compiling select-server.c"
gcc select-server.c  -o server
if [ $? -eq 0 ]; then
  echo $s ok
else
  echo $s fail
  exit 1
fi
s="compiling select-client.c"
gcc select-client.c  -o client
if [ $? -eq 0 ]; then
  echo $s ok
else
  echo $s fail
  exit 1
fi
