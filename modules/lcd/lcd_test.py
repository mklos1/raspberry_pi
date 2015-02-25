import time
from itertools import repeat

f = open('/sys/bus/i2c/devices/1-0027/content', 'w')
while True:
   f.write('LCD DRIVER DEMO\n')
   f.write('  Marcin Klos\n')
   f.flush()
   time.sleep(3)
   for x in range(0, 15):
      f.write(time.strftime("%d %b %Y"))
      f.write('\n')
      f.write(time.strftime("%H:%M:%S"))
      f.write('\n')
      f.flush()
      time.sleep(1)
