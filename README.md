# AMDGPU Fan Control

Fan control based on highest GPU temperature (edge, junction or memory).

    Usage: amdgpufan [options]

    Options:
      -c CONFIG_FILE        Specify config path
      -d --debug            Enable debug logging
      -h --help             Show this help message
      -v --version          Show version


### Building

    make &&& make install

### Configuration

/etc/amdgpufan.conf

```sh
# card
card0
# temperature fanspeed
50 0
60 10
70 30
80 40
90 45
94 50
98 60
```
