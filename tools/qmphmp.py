import socket,json,sys,time
sock=sys.argv[1]
def hmp(cmd):
    s=socket.socket(socket.AF_UNIX); s.connect(sock); f=s.makefile('rw')
    f.readline(); f.write('{"execute":"qmp_capabilities"}\n'); f.flush(); f.readline()
    f.write(json.dumps({"execute":"human-monitor-command","arguments":{"command-line":cmd}})+"\n"); f.flush()
    r=json.loads(f.readline()); s.close(); return r.get('return',r)
for step in sys.argv[2:]:
    if step.startswith('sleep:'): time.sleep(float(step[6:])); continue
    print('>>',step); print(hmp(step))
