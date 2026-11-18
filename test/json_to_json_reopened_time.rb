# spinel: not-cruby -- CRuby's json calls the reopened Time#to_json on a boxed
# Time; spinel's box carries SP_BUILTIN_TIME, not a user class id, so the value
# is its #to_s (the difference this test pins)
# A reopened Range/Time is a builtin, and a builtin value boxes as a tagged
# sp_RbVal naming the builtin's own id (SP_BUILTIN_TIME is -11, SP_BUILTIN_RANGE
# -10), never the reopen's class id. The json hook's switch can therefore never
# reach an arm for the reopen: boxed, a Time serializes as its #to_s, while the
# reopened method does answer on a statically typed receiver. Before mc_method
# gave a Range/Time reopen its `_oc` stem, such an arm also asked for a symbol
# nothing defines (sp_Time_to_json); that build failure is gone, and what this
# pins is the behavior a Range/Time reopen has: the method runs where the type
# is static, and a boxed value is byte-identical to one the hook never saw.
require "json"

class Time
  def to_json(*) = "time:" + to_i.to_s
end
class Range
  def to_json(*) = "range"
end

class Pt
  def to_json(*) = '"PT"'
end

t = Time.new(2020, 1, 2, 3, 4, 5, "+00:00")
r = Range.new(1, 3)

# statically typed receivers reach the reopened methods
p t.to_json
p r.to_json

# boxed, they have no user class id to dispatch on: Time#to_s and Range#to_s,
# as before the reopens (CRuby's json would call the reopened #to_json)
p({ "t" => t, "r" => r, "p" => Pt.new }.to_json)
