# A send whose name is an ivar dispatched over the program's literals alone,
# so an ivar holding a computed name raised NoMethodError for a method the
# receiver answers. The ivar now counts as computed when a write of it in
# the class hierarchy stores one, also through initialize's parameter.

class Task
  def initialize(name) = @name = name
  def run = send(@name)
  def run_copy = (n = @name; public_send(n))
  def step_one = :one
  def step_two = :two
end
p Task.new(["step_", "one"].join).run
p Task.new("step_#{%w[x two].last}".to_sym).run_copy

class Job
  def run = __send__(@action)
  def take(a) = @action = a
  def act_go = :go
end
class LateJob < Job
  def act_late = :late
end
j = LateJob.new
j.take("act_#{%w[late].first}")
p j.run

# handed only literals, it keeps the literal arms
class Plain
  def initialize(name) = @name = name
  def run = send(@name)
  def hello = :hello
end
p Plain.new(:hello).run
