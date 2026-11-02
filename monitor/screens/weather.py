"""Shows the current weather for a city on the Claude Monitor display.

    python3 monitor/cli.py weather "Amsterdam"                  # update once
    python3 monitor/cli.py weather "Amsterdam" --schedule 600   # update every 10 minutes until Ctrl+C

Weather comes from Open-Meteo (https://open-meteo.com), which is free and needs no API key.
The city name is looked up with Open-Meteo's geocoding; add the country if it picks the wrong
place ("Paris, US" doesn't work, but "Paris Texas" usually does), or pass --lat/--lon instead.
"""

from datetime import datetime
from functools import lru_cache
from urllib.parse import urlencode

from display import get_json

TITLE = "Weather"

GEOCODING_URL = "https://geocoding-api.open-meteo.com/v1/search"
FORECAST_URL = "https://api.open-meteo.com/v1/forecast"
HOURS = 24  # how far ahead the temperature graph goes

# WMO weather codes, as Open-Meteo reports them
DESCRIPTIONS = {
    0: "Clear sky", 1: "Mainly clear", 2: "Partly cloudy", 3: "Overcast",
    45: "Fog", 48: "Freezing fog",
    51: "Light drizzle", 53: "Drizzle", 55: "Heavy drizzle",
    56: "Freezing drizzle", 57: "Freezing drizzle",
    61: "Light rain", 63: "Rain", 65: "Heavy rain",
    66: "Freezing rain", 67: "Freezing rain",
    71: "Light snow", 73: "Snow", 75: "Heavy snow", 77: "Snow grains",
    80: "Light showers", 81: "Showers", 82: "Heavy showers",
    85: "Snow showers", 86: "Heavy snow showers",
    95: "Thunderstorm", 96: "Thunderstorm, hail", 99: "Thunderstorm, hail",
}


@lru_cache
def find_city(name):
    """(latitude, longitude, display name) of the best match for `name`."""
    results = get_json(f"{GEOCODING_URL}?{urlencode({'name': name, 'count': 1})}").get("results")
    if not results:
        raise RuntimeError(f"Couldn't find a place called\n{name}")
    place = results[0]
    return place["latitude"], place["longitude"], place["name"]


def fetch_weather(lat, lon):
    query = urlencode({
        "latitude": lat,
        "longitude": lon,
        "current": "temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m",
        "hourly": "temperature_2m,precipitation_probability",
        "timezone": "auto",
        "forecast_hours": HOURS + 1,  # hourly data starts at the current hour
    })
    return get_json(f"{FORECAST_URL}?{query}")


def temperature_color(celsius):
    if celsius < 5:
        return "#66aaff"
    if celsius < 15:
        return "cyan"
    if celsius < 25:
        return "green"
    if celsius < 30:
        return "orange"
    return "red"


def add_arguments(parser):
    parser.add_argument("city", nargs="?", help="city to show, e.g. \"Amsterdam\"")
    parser.add_argument("--lat", type=float, help="latitude, instead of looking up the city")
    parser.add_argument("--lon", type=float, help="longitude, instead of looking up the city")


def check_args(parser, args):
    if not args.city and (args.lat is None or args.lon is None):
        parser.error("give a city name, or --lat and --lon")


def build_screen(args):
    if args.lat is not None and args.lon is not None:
        lat, lon, name = args.lat, args.lon, args.city or TITLE
    else:
        lat, lon, name = find_city(args.city)
    weather = fetch_weather(lat, lon)
    now, hourly = weather["current"], weather["hourly"]
    temp = now["temperature_2m"]
    rain = max((p for p in hourly["precipitation_probability"] if p is not None), default=None)
    # Clock times under the graph, every 6 hours from now
    times = [datetime.fromisoformat(t).strftime("%H:%M") for t in hourly["time"]]
    labels = ["now"] + times[6::6]

    return {
        "title": name,
        "color": "cyan",
        "lines": [
            DESCRIPTIONS.get(now["weather_code"], f"Weather code {now['weather_code']}"),
            {"text": f"{temp:.1f}°C", "size": 4, "color": temperature_color(temp)},
            {"text": f"feels {now['apparent_temperature']:.0f}°C, humidity {now['relative_humidity_2m']}%",
             "color": "grey"},
            {"graph": hourly["temperature_2m"], "color": "heat", "height": 46, "unit": "°",
             "labels": labels, "marker": 0},
            f"rain {rain if rain is not None else '?'}%  wind {now['wind_speed_10m']:.0f} km/h",
            {"text": f"next {HOURS} hours; updated " + datetime.now().strftime("%H:%M"), "size": 1, "color": "grey"},
        ],
    }
