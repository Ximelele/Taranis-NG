"""Authentication manager for the API.

Returns:
    _type_: _description_
"""

from functools import wraps
from flask import request
import os
import ssl

try:
    with open(os.getenv("API_KEY_FILE"), "r") as file:
        api_key = file.read()
except FileNotFoundError:
    print("API_KEY_FILE not found. Please set the API_KEY_FILE environment variable to the path of the file containing the API key.")
    api_key = os.getenv("API_KEY")

if os.getenv("SSL_VERIFICATION") == "False":
    try:
        _create_unverified_https_context = ssl._create_unverified_context
    except AttributeError:
        pass
    else:
        ssl._create_default_https_context = _create_unverified_https_context


def api_key_required(fn):
    """Check if the API key is valid.

    Args:
        fn (function): _description_
    Returns:
        _type_: _description_
    """

    @wraps(fn)
    def wrapper(*args, **kwargs):
        if "Authorization" not in request.headers or request.headers["Authorization"] != ("Bearer " + api_key):
            return {"error": "not authorized"}, 401
        else:
            return fn(*args, **kwargs)

    return wrapper
