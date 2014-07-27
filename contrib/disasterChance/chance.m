##  Copyright (C) 2014  Daniel Kraft <d@domob.eu>
##  Huntercoin.
##
##  This program is free software: you can redistribute it and/or modify
##  it under the terms of the GNU General Public License as published by
##  the Free Software Foundation, either version 3 of the License, or
##  (at your option) any later version.
##
##  This program is distributed in the hope that it will be useful,
##  but WITHOUT ANY WARRANTY; without even the implied warranty of
##  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
##  GNU General Public License for more details.
##
##  You should have received a copy of the GNU General Public License
##  along with this program.  If not, see <http://www.gnu.org/licenses/>.

## Calculate the cummulative chances for a disaster happening depending
## on the chosen parameters in the disaster probability function.

clear ("all");
page_screen_output (false);

% Minimum and maximum time between disasters.
global minTime maxTime;
minTime = 1440;
maxTime = 12 * 1440;

% Set up a time array.
times = 0 : maxTime;

% Set probabilities for disaster at each time separately.
function p = getProb (t)
  global minTime maxTime;

  p = NA (size (t));
  p(t < minTime) = 0;
  p(t >= maxTime) = 1;

  where = (t >= minTime & t < maxTime);
  p(where) = 1 / 10000;
  %p(where) = 1 ./ ((maxTime - minTime) - (t(where) - minTime));
  %p(where) = 1 - exp (1 ./ ((t(where) - minTime) - (maxTime - minTime)));
endfunction
probabilities = getProb (times);

% Cummulate their inverse probabilities to find out the probability
% that a disaster happens somewhere before a given time.
cummNoDisaster = cumprod (1 - probabilities);
disaster = 1 - cummNoDisaster;

% Plot it.
plot (times, disaster);
