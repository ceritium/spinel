# `require "json"` plus a reopened core class with its own #to_json is how an
# app shapes its JSON (activesupport's core_ext does the same). The generated
# sp_obj_to_json hook switches on a class id and casts v.v.p to that class's
# struct; a reopened primitive is neither: its values are tagged sp_RbVal
# (SP_TAG_INT, SP_TAG_STR) carrying the builtin's id, so an arm for the
# reopen could never match -- and for Integer the cast named a struct that
# does not exist:
#
#   class Integer; def to_json(*) = to_s; end
#   error: use of undeclared identifier 'sp_Integer'
#
# Marshal.load marks every class instantiated (a `.new` on a Class value that
# cannot be resolved), which is what put the reopens in the hook's arm list;
# without it the class was skipped and the bug stayed hidden. The reopen's
# method still answers on a statically typed receiver -- 7.to_json below --
# while a value the JSON walk reaches boxed serializes as it did before the
# reopen, which is the price of the tagged representation.
require "json"

class Integer
  def to_json(*) = "int:" + to_s
end
class String
  def to_json(*) = "str:" + self
end

class Pt
  def initialize; @x = 1; end
  def to_json(*) = '"pt:' + @x.to_s + '"'
end

# a statically typed receiver: the reopened method answers
p 7.to_json
p "s".to_json

# the instantiated class list the hook walks, with every reopen in it: the
# build is the assertion, and the boxed values below are byte-identical to
# what they serialized to before the reopens existed
pt = Marshal.load(Marshal.dump(Pt.new))
p({ "n" => 1, "s" => "s", "p" => pt }.to_json)
