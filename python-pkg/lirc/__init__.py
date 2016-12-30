'''  This file marks the directory as importable. '''

# pylint: disable=wrong-import-position, wrong-import-order

import sys
import os.path
sys.path.insert(0, os.path.abspath(os.path.dirname(__file__)))

from client import get_default_lircrc_path
from client import get_default_socket_path

from async_client import AsyncConnection
from client import TimeoutException
from client import RawConnection
from client import CommandConnection
from client import LircdConnection
from client import Command
from client import Reply

from client import DrvOptionCommand
from client import ListKeysCommand
from client import ListRemotesCommand
from client import SendCommand
from client import SetLogCommand
from client import SetTransmittersCommand
from client import SimulateCommand
from client import StartRepeatCommand
from client import StopRepeatCommand
from client import VersionCommand

from _client import lirc_deinit            # pylint: disable=no-name-in-module

sys.path.pop(0)
