#!/usr/bin/env python
# Copyright (c) 2012 Cloudera, Inc. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Hdfs access utilities

from os import environ
from os.path import join as join_path
from pywebhdfs.webhdfs import PyWebHdfsClient, errors, _raise_pywebhdfs_exception
from xml.etree.ElementTree import parse
import getpass
import httplib
import requests
import types

class PyWebHdfsClientWithChmod(PyWebHdfsClient):
  def chmod(self, path, permission):
    """Set the permission of 'path' to 'permission' (specified as an octal string, e.g.
    '775'"""
    uri = self._create_uri(path, "SETPERMISSION", permission=permission)
    response = requests.put(uri, allow_redirects=True)
    if not response.status_code == httplib.OK:
      _raise_pywebhdfs_exception(response.status_code, response.text)
    return True

  def chown(self, path, user, group):
    """Sets the owner and the group of 'path to 'user' / 'group'"""
    uri = self._create_uri(path, "SETOWNER", owner=user, group=group)
    response = requests.put(uri, allow_redirects=True)
    if not response.status_code == httplib.OK:
      _raise_pywebhdfs_exception(response.status_code, response.text)
    return True

  def setacl(self, path, acls):
    uri = self._create_uri(path, "SETACL", aclspec=acls)
    response = requests.put(uri, allow_redirects=True)
    if not response.status_code == httplib.OK:
      _raise_pywebhdfs_exception(response.status_code, response.text)
    return True

  def getacl(self, path):
    uri = self._create_uri(path, "GETACLSTATUS")
    response = requests.get(uri, allow_redirects=True)
    if not response.status_code == httplib.OK:
      _raise_pywebhdfs_exception(response.status_code, response.text)
    return response.json()

class HdfsConfig(object):
  """Reads an XML configuration file (produced by a mini-cluster) into a dictionary
  accessible via get()"""
  def __init__(self, *filename):
    self.conf = {}
    for arg in filename:
      tree = parse(arg)
      for property in tree.getroot().getiterator('property'):
        self.conf[property.find('name').text] = property.find('value').text

  def get(self, key):
    return self.conf.get(key)

def get_hdfs_client_from_conf(conf):
  """Returns a new HTTP client for an HDFS cluster using an HdfsConfig object"""
  hostport = conf.get('dfs.namenode.http-address')
  if hostport is None:
    raise Exception("dfs.namenode.http-address not found in config")
  host, port = hostport.split(":")
  return get_hdfs_client(host=host, port=port)

def __pyweb_hdfs_client_exists(self, path):
  """The PyWebHdfsClient doesn't provide an API to cleanly detect if a file or directory
  exists. This method is bound to each client that is created so tests can simply call
  hdfs_client.exists('path') and get back a bool.
  """
  try:
    self.get_file_dir_status(path)
  except errors.FileNotFound:
    return False
  return True

def get_hdfs_client(host, port, user_name=getpass.getuser()):
  """Returns a new HTTP client for an HDFS cluster using an explict host:port pair"""
  hdfs_client = PyWebHdfsClientWithChmod(host=host, port=port, user_name=user_name)
  # Bind our "exists" method to hdfs_client.exists
  hdfs_client.exists = types.MethodType(__pyweb_hdfs_client_exists, hdfs_client)
  return hdfs_client

def get_default_hdfs_config():
  core_site_path = join_path(environ.get('HADOOP_CONF_DIR'), 'core-site.xml')
  hdfs_site_path = join_path(environ.get('HADOOP_CONF_DIR'), 'hdfs-site.xml')
  return HdfsConfig(core_site_path, hdfs_site_path)

def create_default_hdfs_client():
  return get_hdfs_client_from_conf(get_default_hdfs_config())
